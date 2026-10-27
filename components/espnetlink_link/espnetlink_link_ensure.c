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
 * @file espnetlink_link_ensure.c
 * @brief The dongle preferences the WiCAN enforces over the cable during
 *        the pairing pass (2026-10-19, Ali): AGNSS on (the dongle firmware
 *        ships it off since 2026-10-18, `gps.agnss_enabled`), and in
 *        wifi_modem mode the dongle's WiFi access point on (shipped off
 *        since the same day, `wifi_manager.mode`: a fresh dongle keeps its
 *        radio quiet until a host asks for it). Both are reboot-to-apply
 *        on the dongle: the values are staged with the dongle's own
 *        settings API and submitted once, the dongle restarts once, and
 *        the pass starts over after the re-enumeration (the same contract
 *        as the sharing / USB class pass of 2026-10-18).
 *
 * A dongle firmware without the key or the component (an older build)
 * answers 404 or lacks the key: it is left alone, with a D line.
 *
 * The carrier settings relay lives here too (2026-10-07, Quick Setup's USB
 * step; moved from the USB driver on 2026-10-28 when the USB share pass
 * started calling it as well): the WiCAN's APN, user, password and SIM PIN
 * onto the dongle's `lte_upstream_pppos`, the same stage-then-submit
 * contract.
 */
#include "espnetlink_link_private.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "http_client_manager.h"

static const char *TAG = "espnetlink";

/** The dongle's wifi_manager.mode as it stands, "" when the document has
 *  no such key. False on a transport failure. @p status_out receives the
 *  HTTP status. */
static bool read_wifi_mode(char *out, size_t cap, int *status_out)
{
    http_client_response_t resp;

    out[0] = '\0';

    if (!espnl_usb_request("/api/settings/wifi_manager", HTTP_CLIENT_GET,
                           NULL, &resp))
    {
        *status_out = -1;
        return false;
    }

    *status_out = resp.status_code;

    cJSON *obj = (resp.status_code == 200 && resp.data != NULL)
                     ? cJSON_Parse(resp.data) : NULL;

    http_client_manager_free(&resp);

    if (cJSON_IsObject(obj))
    {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(obj, "mode");

        if (cJSON_IsString(m) && m->valuestring != NULL)
        {
            strlcpy(out, m->valuestring, cap);
        }
    }

    cJSON_Delete(obj);
    return true;
}

int espnl_ensure_prefs(bool want_ap, bool submit)
{
    bool changed = false;

    /* 1. AGNSS: assistance data is what makes a cold fix take a minute
       instead of ten on the window bench; the WiCAN wants it on */
    int r = espnl_usb_ensure_setting("/api/settings/gps", "agnss_enabled",
                                     cJSON_CreateBool(true));

    if (r == -2)
    {
        ESP_LOGD(TAG, "dongle has no gps settings API: AGNSS left to it");
        espnl_engine_note_prefs("unsupported", NULL);
    }
    else if (r < 0)
    {
        return -1;
    }
    else if (r == 1)
    {
        ESP_LOGI(TAG, "dongle AGNSS was off: enabling it");
        espnl_engine_note_prefs("enabled", NULL);
        changed = true;
    }
    else
    {
        espnl_engine_note_prefs("on", NULL);
    }

    /* 2. the access point, only when this WiCAN joins it (wifi_modem) */
    if (!want_ap)
    {
        espnl_engine_note_prefs(NULL, "not_managed");
    }
    else
    {
        char mode[12];
        int status = 0;

        if (!read_wifi_mode(mode, sizeof(mode), &status))
        {
            return -1;
        }

        if (status == 404 || mode[0] == '\0')
        {
            ESP_LOGD(TAG, "dongle has no wifi_manager settings API: its AP "
                          "is left as it is");
            espnl_engine_note_prefs(NULL, "unsupported");
        }
        else
        {
            const char *want = espnl_core_ap_mode_wanted(mode);

            if (want == NULL)
            {
                espnl_engine_note_prefs(NULL, "on");
            }
            else
            {
                r = espnl_usb_ensure_setting("/api/settings/wifi_manager",
                                             "mode", cJSON_CreateString(want));

                if (r < 0 && r != -2)
                {
                    return -1;
                }

                if (r == 1)
                {
                    ESP_LOGI(TAG, "dongle WiFi was %s: enabling its access "
                                  "point (mode %s) for the WiFi modem path",
                             mode[0] ? mode : "unset", want);
                    espnl_engine_note_prefs(NULL, "enabled");
                    changed = true;
                }
                else if (r == 0)
                {
                    espnl_engine_note_prefs(NULL, "on");
                }
            }
        }
    }

    if (!changed)
    {
        return 0;
    }

    if (submit)
    {
        if (!espnl_usb_submit())
        {
            ESP_LOGW(TAG, "dongle preferences staged but the submit failed");
            return -1;
        }

        ESP_LOGI(TAG, "dongle preferences sent: it restarts once to apply "
                      "them");
    }

    return 1;
}

/* ---- the carrier settings (2026-10-07; both passes since 2026-10-28) ----- */

int espnl_ensure_carrier(const char *sim_state, bool *pin_sent, bool submit)
{
    const espnl_config_t *cfg = espnl_config();
    http_client_response_t resp;
    bool want_apn = cfg->apn[0] != '\0' || cfg->apn_user[0] != '\0';
    bool pin_due = espnl_core_pin_relay_due(cfg->sim_pin, sim_state,
                                            pin_sent != NULL && *pin_sent);

    if (!want_apn && !pin_due)
    {
        espnl_engine_note_carrier(true, "");
        return 0;
    }
    if (!espnl_usb_request("/api/settings/lte_upstream_pppos",
                           HTTP_CLIENT_GET, NULL, &resp))
    {
        return -1;
    }
    if (resp.status_code == 404)
    {
        http_client_manager_free(&resp);
        ESP_LOGW(TAG, "carrier settings: the dongle firmware has no LTE "
                 "settings API (404): update the dongle firmware");
        espnl_engine_note_carrier(false, "the dongle firmware has no LTE "
                                         "settings API: update it");
        return -2;
    }

    cJSON *obj = (resp.status_code == 200 && resp.data != NULL)
                     ? cJSON_Parse(resp.data) : NULL;

    http_client_manager_free(&resp);
    if (!cJSON_IsObject(obj))
    {
        cJSON_Delete(obj);
        return -1;
    }

    /* the GET returns the PERSISTED (staged) document, see the pure rule */
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(obj, "apn");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(obj, "apn_user");
    espnl_carrier_verdict_t v = espnl_core_carrier_verdict(
        cJSON_IsString(a) ? a->valuestring : NULL,
        cJSON_IsString(u) ? u->valuestring : NULL,
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "pending_reboot")),
        cfg->apn, cfg->apn_user);

    if (v != ESPNL_CARRIER_DIFFERS && v != ESPNL_CARRIER_STAGED && !pin_due)
    {
        /* the dongle holds them, or there is nothing of ours to hold */
        cJSON_Delete(obj);
        espnl_engine_note_carrier(true, "");
        return 0;
    }

    if (v == ESPNL_CARRIER_DIFFERS || pin_due)
    {
        /* §6 full-object replace; a "" password (or PIN) keeps the dongle's */
        if (v == ESPNL_CARRIER_DIFFERS)
        {
            cJSON_DeleteItemFromObjectCaseSensitive(obj, "apn");
            cJSON_AddStringToObject(obj, "apn", cfg->apn);
            cJSON_DeleteItemFromObjectCaseSensitive(obj, "apn_user");
            cJSON_AddStringToObject(obj, "apn_user", cfg->apn_user);
            cJSON_DeleteItemFromObjectCaseSensitive(obj, "apn_password");
            cJSON_AddStringToObject(obj, "apn_password", cfg->apn_password);
        }
        if (pin_due)
        {
            cJSON_DeleteItemFromObjectCaseSensitive(obj, "apn_pin");
            cJSON_AddStringToObject(obj, "apn_pin", cfg->sim_pin);
        }

        char *body = cJSON_PrintUnformatted(obj);

        cJSON_Delete(obj);
        if (body == NULL)
        {
            return -1;
        }

        bool ok = espnl_usb_request("/api/settings/lte_upstream_pppos",
                                    HTTP_CLIENT_PUT, body, &resp);
        int status = ok ? resp.status_code : -1;

        free(body);
        if (ok)
        {
            http_client_manager_free(&resp);
        }
        if (status != 200)
        {
            ESP_LOGW(TAG, "carrier settings: PUT status %d", status);
            espnl_engine_note_carrier(false, "the dongle refused the "
                                             "carrier settings");
            return -1;
        }
    }
    else
    {
        /* staged by an earlier pass whose submit never landed (the trap
         * espnl_usb_ensure_setting met on 2026-08-25): the submit alone
         * is owed */
        cJSON_Delete(obj);
        ESP_LOGW(TAG, "carrier settings staged on the dongle but not "
                 "applied (pending_reboot): submitting");
    }

    if (pin_due && pin_sent != NULL)
    {
        *pin_sent = true;   /* once per boot, whatever the SIM says of it */
    }

    if (submit && !espnl_usb_submit())
    {
        espnl_engine_note_carrier(false, "the dongle did not take the "
                                         "settings submit");
        return -1;
    }
    ESP_LOGI(TAG, "carrier settings %s the dongle (APN '%s'%s%s%s): it "
             "restarts once to apply them", submit ? "sent to" : "staged on",
             cfg->apn, cfg->apn_user[0] ? ", user " : "", cfg->apn_user,
             pin_due ? ", the SIM PIN" : "");
    espnl_engine_note_carrier(false, "sent to the dongle, which restarts to "
                                     "apply them");
    return 1;
}
