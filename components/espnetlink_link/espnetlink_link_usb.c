/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file espnetlink_link_usb.c
 * @brief The USB side of the zero-touch pairing: drives the pure state
 *        machine (espnetlink_link_core.c) from usb_host_manager's status
 *        and performs its actions — HTTP to the dongle at 192.168.7.1
 *        over the NCM link (the netif's subnet route; no default-route
 *        games), the settings store, the data-line cut, VBUS recovery,
 *        and the one reboot-to-apply after a new key.
 *
 * Derived, not event-driven: every tick (link task, 1 s) reads
 * usb_host_manager_status() — an NCM link up with an IP and the
 * ESPNetLink's VID/PID is the "attach" edge; its loss is the "drop".
 * Budget (contract §1): dongle boot -> cut in <= 10 s; identify + key +
 * cut are three small HTTP round-trips on a freshly enumerated link.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "http_client_manager.h"
#include "restart_tracker.h"
#include "usb_host_manager.h"
#include "wifi_manager.h"

#include "espnetlink_link.h"
#include "espnetlink_link_core.h"
#include "espnetlink_link_private.h"

static const char *TAG = "espnetlink";

#define USB_HTTP_TIMEOUT_MS   3000
#define USB_MAX_RESP          2048
#define USB_VBUS_OFF_MS       1000   /* long enough for the dongle's caps */
#define USB_REBOOT_DELAY_MS   2000   /* let the cut land + logs flush      */

static espnl_sm_t s_sm;
static bool s_was_up;
static bool s_attached;      /* NCM up to a 303A:4007 device            */
static bool s_repair_req;
static uint32_t s_cuts;
static uint32_t s_cycles;
static uint32_t s_errors;
static bool s_reboot_spawned;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- HTTP to the dongle over USB ---------------------------------------- */

static bool usb_request(const char *path, http_client_method_t method,
                        const char *body, http_client_response_t *resp)
{
    char url[80];

    if (!espnl_core_url(url, sizeof(url), ESPNL_USB_HOST_ADDR, path))
    {
        return false;
    }

    http_client_request_t req =
    {
        .url          = url,
        .method       = method,
        .body         = body,
        .body_len     = body != NULL ? strlen(body) : 0,
        .content_type = "application/json",
        .timeout_ms   = USB_HTTP_TIMEOUT_MS,
        .max_response = USB_MAX_RESP,
    };

    memset(resp, 0, sizeof(*resp));

    esp_err_t err = http_client_manager_request(&req, resp);

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "usb %s: %s", path, esp_err_to_name(err));
        http_client_manager_free(resp);
        return false;
    }
    ESP_LOGD(TAG, "usb %s -> %d", path, resp->status_code);
    return true;
}

/* ---- actions ------------------------------------------------------------ */

static espnl_sm_event_t act_get_info(void)
{
    http_client_response_t resp;
    espnl_info_t info;

    if (!usb_request("/api/info", HTTP_CLIENT_GET, NULL, &resp))
    {
        return ESPNL_EV_FAIL;
    }

    bool parsed = resp.status_code == 200 && resp.data != NULL &&
                  espnl_core_parse_info(resp.data, &info);

    http_client_manager_free(&resp);

    if (!parsed)
    {
        return ESPNL_EV_FAIL;
    }
    if (!info.is_espnetlink)
    {
        ESP_LOGI(TAG, "USB device answers /api/info but is not an "
                 "ESPNetLink");
        return ESPNL_EV_NOT_ESPNETLINK;
    }

    ESP_LOGI(TAG, "identified ESPNetLink %s (fw %s, api %d)", info.device_id,
             info.fw_version, info.api_level);
    espnl_engine_note_info(info.device_id, info.fw_version, info.api_level);
    if (info.api_level < ESPNL_MIN_API_LEVEL)
    {
        /* still probed: dongle builds between the routes and the level
         * bump exist. A 404 below turns this into UNSUPPORTED with the
         * exact reason; the status shows the level either way. */
        ESP_LOGW(TAG, "dongle firmware %s reports API level %d; this WiCAN "
                 "expects %d - update the dongle firmware if pairing, the "
                 "USB class or the LTE health stay unavailable",
                 info.fw_version, info.api_level, ESPNL_MIN_API_LEVEL);
    }
    return ESPNL_EV_OK;
}

static espnl_sm_event_t act_get_key(void)
{
    http_client_response_t resp;
    espnl_creds_t creds;

    if (!usb_request("/api/wifi_modem/credentials", HTTP_CLIENT_GET, NULL,
                     &resp))
    {
        return ESPNL_EV_FAIL;
    }

    bool parsed = resp.status_code == 200 && resp.data != NULL &&
                  espnl_core_parse_credentials(resp.data, &creds);
    int status = resp.status_code;

    http_client_manager_free(&resp);

    if (status == 404)
    {
        /* bench 2026-09-08: a July test build on the dongle answered
         * /api/info but had no WiFi-modem routes at all. Deterministic:
         * no retry, no VBUS cycle, say what to do. */
        ESP_LOGW(TAG, "credentials: HTTP 404 - the dongle firmware has no "
                 "WiFi-modem API; update the dongle firmware");
        espnl_status_set_last_error("the dongle firmware has no WiFi-modem "
                                    "API (credentials: HTTP 404): update "
                                    "the dongle firmware, then plug it in "
                                    "again");
        return ESPNL_EV_UNSUPPORTED;
    }
    if (!parsed)
    {
        /* 403 = the dongle thinks we arrived over WiFi: not a USB link */
        ESP_LOGW(TAG, "credentials: status %d", status);
        return ESPNL_EV_FAIL;
    }

    if (espnl_config()->ssid[0] == '\0' &&
        wifi_manager_ap_password_is_factory())
    {
        /* a fresh WiCAN: the store would write wifi_manager and its
         * factory-password gate (2026-09-07) refuses that. Park instead
         * of churning; the password change restarts the device and the
         * next enumeration pairs. */
        memset(creds.password, 0, sizeof(creds.password));
        ESP_LOGW(TAG, "zero-touch pairing is ON HOLD (factory AP password): "
                 "set a new AP password in the web UI; the restart then "
                 "completes the pairing by itself");
        espnl_status_set_last_error("pairing is on hold: the access point "
                                    "still has the factory password. Set a "
                                    "new AP password; the device restarts "
                                    "and pairs by itself");
        return ESPNL_EV_HOLD;
    }

    int slot = -1;
    bool changed = false;
    esp_err_t err = espnl_pair_store(creds.ssid, creds.password,
                                     creds.device_id, &slot, &changed);

    /* never log the key */
    memset(creds.password, 0, sizeof(creds.password));

    if (err != ESP_OK)
    {
        /* the store already recorded why (last_error): a settings-gate
         * refusal (a paired WiCAN with the factory AP password meeting a
         * re-provisioned dongle) or no free WiFi slot. Neither a retry
         * nor a dongle power cycle changes that: park. */
        ESP_LOGE(TAG, "credentials: store failed: %s - pairing on hold "
                 "until the settings allow it", esp_err_to_name(err));
        return ESPNL_EV_HOLD;
    }

    s_sm.reboot_pending = changed;
    ESP_LOGI(TAG, "credentials ok: '%s' (%s, wifi slot %d)%s", creds.ssid,
             creds.device_id, slot,
             changed ? " — stored; reboot after the cut" : " — unchanged");
    return ESPNL_EV_OK;
}

static espnl_sm_event_t act_post_cut(void)
{
    http_client_response_t resp;

    if (!usb_request("/api/wifi_modem/usb_data", HTTP_CLIENT_POST,
                     "{\"enabled\":false}", &resp))
    {
        return ESPNL_EV_FAIL;
    }

    int status = resp.status_code;

    http_client_manager_free(&resp);

    if (status == 200)
    {
        ESP_LOGI(TAG, "usb_data off requested (cut in ~500 ms)");
        return ESPNL_EV_OK;
    }
    if (status == 409)
    {
        ESP_LOGW(TAG, "dongle locked usb_mode=ncm (tether mode): leaving "
                 "USB data on");
        return ESPNL_EV_CONFLICT;
    }
    ESP_LOGW(TAG, "usb_data off: status %d", status);
    return ESPNL_EV_FAIL;
}

/* GET /api/settings/<component>; when `key` differs from `want`, PUT the
 * full object back with key=want (§6: full-object replace — the
 * transport's synthetic GET keys are stripped server-side, "" passwords
 * keep the stored value). Takes ownership of `want`.
 * Returns 1 = changed (submit needed), 0 = already right, -1 = error,
 * -2 = the dongle has no such settings component (404). */
static int ensure_dongle_setting(const char *path, const char *key,
                                 cJSON *want)
{
    http_client_response_t resp;

    if (!usb_request(path, HTTP_CLIENT_GET, NULL, &resp))
    {
        cJSON_Delete(want);
        return -1;
    }

    if (resp.status_code == 404)
    {
        http_client_manager_free(&resp);
        cJSON_Delete(want);
        return -2;
    }

    cJSON *obj = (resp.status_code == 200 && resp.data != NULL)
                     ? cJSON_Parse(resp.data) : NULL;

    http_client_manager_free(&resp);

    if (!cJSON_IsObject(obj))
    {
        cJSON_Delete(obj);
        cJSON_Delete(want);
        return -1;
    }

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cur != NULL && cJSON_Compare(cur, want, true))
    {
        /* the GET returns the PERSISTED (staged) document: a value that
         * matches but carries pending_reboot=true was staged by an
         * earlier pass whose submit never landed — without a submit the
         * dongle would run the OLD value forever (bench-hit 2026-08-25:
         * an interrupted class change left class=ncm staged over a live
         * RNDIS device). Report "changed" so the caller submits. */
        bool pending = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
            obj, "pending_reboot"));

        cJSON_Delete(obj);
        cJSON_Delete(want);
        if (pending)
        {
            ESP_LOGW(TAG, "dongle %s %s staged but not applied "
                     "(pending_reboot): submitting", path, key);
            return 1;
        }
        return 0;
    }

    if (cur != NULL)
    {
        cJSON_ReplaceItemInObjectCaseSensitive(obj, key, want);
    }
    else
    {
        cJSON_AddItemToObject(obj, key, want);
    }

    char *body = cJSON_PrintUnformatted(obj);

    cJSON_Delete(obj);
    if (body == NULL)
    {
        return -1;
    }

    bool ok = usb_request(path, HTTP_CLIENT_PUT, body, &resp);
    int status = ok ? resp.status_code : -1;

    free(body);
    if (ok)
    {
        http_client_manager_free(&resp);
    }
    if (status != 200)
    {
        ESP_LOGW(TAG, "dongle %s %s PUT: status %d", path, key, status);
        return -1;
    }

    return 1;
}

/* usb_ncm / usb_rndis mode: make the dongle's USB link an internet path.
 * Two settings on the dongle side: `lte_upstream_pppos.ncm_share` (its USB
 * DHCP offers router/DNS + NAPT) and `usb_dev_ethernet.class` (which USB
 * Ethernet class it presents — must match our mode). Read both; when
 * either differs, PUT + submit ONCE (the dongle reboots once to apply). */
static espnl_sm_event_t act_ensure_share(void)
{
    const char *cls = espnl_core_mode_usb_class(
        (espnl_core_mode_t)espnl_config()->mode);
    bool changed = false;
    bool unsupported = false;

    int r = ensure_dongle_setting("/api/settings/lte_upstream_pppos",
                                  "ncm_share", cJSON_CreateBool(true));

    if (r == -2)
    {
        /* no LTE settings component at all: a dongle build from before
         * the WiFi-modem work. The link stays a link; internet over it
         * cannot be arranged from here. */
        ESP_LOGW(TAG, "dongle firmware has no LTE settings API (HTTP 404): "
                 "it cannot share LTE over USB - update the dongle firmware");
        espnl_status_set_last_error("the dongle firmware has no LTE "
                                    "settings API (HTTP 404): it cannot "
                                    "share LTE over USB. Update the dongle "
                                    "firmware");
        return ESPNL_EV_UNSUPPORTED;
    }
    if (r < 0)
    {
        return ESPNL_EV_FAIL;
    }
    changed = (r == 1);

    r = ensure_dongle_setting("/api/settings/usb_dev_ethernet", "class",
                              cJSON_CreateString(cls != NULL ? cls : "ncm"));
    if (r == -2)
    {
        /* pre-2026-08-25 dongle firmware: the class is compile-time there
         * (NCM). The uplink still works — just not as the asked class. */
        if (cls != NULL && strcmp(cls, "ncm") != 0)
        {
            ESP_LOGW(TAG, "dongle firmware cannot select the USB class "
                     "(no usb_dev_ethernet settings): it stays on its "
                     "built-in default - update the dongle firmware");
            espnl_status_set_last_error("the dongle firmware cannot select "
                                        "the USB class (no usb_dev_ethernet "
                                        "settings): it stays on CDC-NCM. "
                                        "Update the dongle firmware");
            unsupported = true;
        }
    }
    else if (r < 0)
    {
        return ESPNL_EV_FAIL;
    }
    else
    {
        changed = changed || (r == 1);
    }

    if (!changed)
    {
        ESP_LOGI(TAG, "dongle USB uplink already configured (class %s, "
                 "ncm_share on)", cls != NULL ? cls : "ncm");
        if (!unsupported)
        {
            espnl_status_set_last_error("");
        }
        return unsupported ? ESPNL_EV_UNSUPPORTED : ESPNL_EV_OK;
    }

    http_client_response_t resp;
    bool ok = usb_request("/api/settings/submit", HTTP_CLIENT_POST, "{}",
                          &resp);
    int status = ok ? resp.status_code : -1;

    if (ok)
    {
        http_client_manager_free(&resp);
    }
    if (status != 200)
    {
        ESP_LOGW(TAG, "dongle settings submit: status %d", status);
        return ESPNL_EV_FAIL;
    }

    ESP_LOGI(TAG, "dongle USB uplink reconfigured (class %s, ncm_share on) "
             "— it reboots to apply", cls != NULL ? cls : "ncm");
    if (!unsupported)
    {
        espnl_status_set_last_error("");
    }
    return ESPNL_EV_OK;
}

static void act_vbus_cycle(void)
{
    esp_err_t err = usb_host_manager_set_vbus(false);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "vbus cycle: %s", esp_err_to_name(err));
        return;
    }
    espnl_engine_dongle_rebooting(); /* its AP forgets us: re-join later */
    vTaskDelay(pdMS_TO_TICKS(USB_VBUS_OFF_MS));
    (void)usb_host_manager_set_vbus(true);
    s_cycles++;
    ESP_LOGI(TAG, "dongle VBUS cycled (recovery #%lu)",
             (unsigned long)s_cycles);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(USB_REBOOT_DELAY_MS));
    restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY,
                            RESTART_TRACKER_SOURCE_PAIRING, 0);
}

static void act_reboot(void)
{
    if (s_reboot_spawned)
    {
        return;
    }
    s_reboot_spawned = true;
    ESP_LOGW(TAG, "new dongle key stored: rebooting to apply (once)");

    /* internal-RAM stack (default heap): the task ends in esp_restart() */
    if (xTaskCreate(reboot_task, "espnl_reboot", 3072, NULL, 5, NULL)
            != pdPASS)
    {
        ESP_LOGE(TAG, "reboot task alloc failed; rebooting inline");
        restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY,
                                RESTART_TRACKER_SOURCE_PAIRING, 0);
    }
}

/* ---- machine driver ----------------------------------------------------- */

static void run(espnl_sm_event_t ev)
{
    /* an action's outcome is fed straight back; bounded so a confused
     * table can never spin the task */
    for (int guard = 0; guard < 8; guard++)
    {
        espnl_sm_state_t before = s_sm.state;
        espnl_sm_action_t act = espnl_sm_step(&s_sm, ev, now_ms());

        if (s_sm.state != before)
        {
            ESP_LOGD(TAG, "pair: %s -> %s", espnl_sm_state_str(before),
                     espnl_sm_state_str(s_sm.state));
            if (s_sm.state == ESPNL_SM_DONE)
            {
                s_cuts++;
                ESP_LOGI(TAG, "usb gone: cable is power only (cut #%lu)",
                         (unsigned long)s_cuts);
            }
            else if ((s_sm.state == ESPNL_SM_FOREIGN ||
                      s_sm.state == ESPNL_SM_UNSUPPORTED) &&
                     before != ESPNL_SM_IDLE)
            {
                s_errors++;
            }
        }

        switch (act)
        {
            case ESPNL_ACT_NONE:
                return;
            case ESPNL_ACT_GET_INFO:
                ev = act_get_info();
                break;
            case ESPNL_ACT_GET_KEY:
                ev = act_get_key();
                break;
            case ESPNL_ACT_POST_CUT:
                ev = act_post_cut();
                break;
            case ESPNL_ACT_ENSURE_SHARE:
                ev = act_ensure_share();
                break;
            case ESPNL_ACT_VBUS_CYCLE:
                act_vbus_cycle();
                return;
            case ESPNL_ACT_REBOOT:
                act_reboot();
                return;
            case ESPNL_ACT_GIVE_UP:
                ESP_LOGW(TAG, "pairing gave up until the dongle re-plugs "
                         "(%lu VBUS cycles)", (unsigned long)s_cycles);
                return;
            default:
                return;
        }
    }
}

void espnl_usb_init(void)
{
    const espnl_config_t *cfg = espnl_config();

    espnl_sm_init(&s_sm, cfg->mode != ESPNETLINK_MODE_WIFI_MODEM,
                  cfg->cut_retries, now_ms());
    s_was_up = false;
    s_attached = false;
    s_repair_req = false;
}

void espnl_usb_tick(bool ap_stale)
{
    const espnl_config_t *cfg = espnl_config();
    usb_host_manager_status_t st;

    if (usb_host_manager_status(&st) != ESP_OK)
    {
        return;
    }

    bool up = st.host_active && st.eth_connected && st.ip[0] != '\0';
    bool is_espnl = up && espnl_core_is_espnetlink(st.vid, st.pid);

    s_attached = is_espnl;

    if (up != s_was_up)
    {
        s_was_up = up;
        if (!cfg->auto_pair)
        {
            /* manual mode: never identify/cut; the link is just a link */
        }
        else if (up)
        {
            ESP_LOGI(TAG, "usb link up (%04x:%04x, %s)", st.vid, st.pid,
                     st.ip);
            /* the factory-password hold (2026-09-07) is decided AFTER the
             * identify + key read (act_get_key -> HOLD): the status then
             * shows which dongle/firmware is there, and nothing is cut */
            if (is_espnl)
            {
                /* a fresh enumeration = the dongle (re)booted: the AP
                 * association we may hold is dead (bench 2026-08-24) */
                espnl_engine_dongle_rebooting();
            }
            run(is_espnl ? ESPNL_EV_LINK_UP : ESPNL_EV_LINK_UP_OTHER);
            return; /* the attach edge already ran the first actions */
        }
        else
        {
            run(ESPNL_EV_LINK_DOWN);
            return;
        }
    }

    if (!cfg->auto_pair)
    {
        return;
    }

    if (s_repair_req)
    {
        s_repair_req = false;
        run(ESPNL_EV_REPAIR);
        return;
    }
    if (ap_stale && s_sm.state == ESPNL_SM_DONE)
    {
        ESP_LOGW(TAG, "stored key no longer joins the dongle AP: "
                 "re-reading it over USB");
        run(ESPNL_EV_AP_STALE);
        return;
    }

    run(ESPNL_EV_TICK);
}

void espnl_usb_status(espnl_usb_status_t *out)
{
    out->attached = s_attached;
    out->state = espnl_sm_state_str(s_sm.state);
    out->ncm_steady = s_sm.state == ESPNL_SM_NCM_UP ||
                      (s_attached && !espnl_config()->auto_pair);
    out->cuts = s_cuts;
    out->vbus_cycles = s_cycles;
    out->errors = s_errors;
}

esp_err_t espnl_usb_repair(void)
{
    usb_host_manager_status_t st;

    if (usb_host_manager_status(&st) != ESP_OK || !st.host_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_repair_req = true; /* picked up by the next tick (task context) */
    return ESP_OK;
}

const char *espnl_usb_host(void)
{
    return s_attached ? ESPNL_USB_HOST_ADDR : "";
}
