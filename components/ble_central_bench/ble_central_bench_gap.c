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
 * @file ble_central_bench_gap.c
 * @brief The NimBLE central: scan for the target name, connect, tune the
 *        link like esp-idf's throughput_app (DLE 251, preferred MTU,
 *        connection interval, PHY), pair with the fixed passkey
 *        (KeyboardOnly, passkey injected), hand the secured link to the
 *        discovery file, and offer blocking GATT primitives to the bench
 *        task. Every GAP callback only flags, counts or copies into the
 *        core.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "host/ble_esp_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"

#include "ble_central_bench_private.h"

static const char *TAG = BCB_TAG;

void ble_store_config_init(void);

/* ---- state ------------------------------------------------------------------------ */

static uint8_t  s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected;
static bool     s_synced;
static char     s_peer_name[32];

/* blocking GATT ops: one at a time from the bench task */
static SemaphoreHandle_t s_op_done;
static int      s_op_status;
static uint8_t *s_op_dst;
static size_t   s_op_cap, s_op_len;

static int gap_event(struct ble_gap_event *event, void *arg);

/* ---- helpers ---------------------------------------------------------------------- */

static void addr_str(const ble_addr_t *a, char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static void report_link(void)
{
    struct ble_gap_conn_desc d;
    uint8_t tx = 0, rx = 0;

    if (ble_gap_conn_find(s_conn, &d) != 0)
    {
        return;
    }

    (void)ble_gap_read_le_phy(s_conn, &tx, &rx);
    bcb_core_on_link_params(ble_att_mtu(s_conn), d.conn_itvl, tx, rx, 0, 0);
}

static void tune_link(void)
{
    const bcb_config_t *cfg = bcb_settings_config();
    int rc;

    if (cfg->dle)
    {
        rc = ble_hs_hci_util_set_data_len(s_conn, BCB_DLE_OCTETS, BCB_DLE_TIME_US);

        if (rc != 0)
        {
            bcb_diag("DLE request refused (rc %d)", rc);
        }
    }

    rc = ble_gattc_exchange_mtu(s_conn, NULL, NULL);

    if (rc != 0)
    {
        ESP_LOGW(TAG, "MTU exchange failed (%d)", rc);
    }

    struct ble_gap_upd_params p =
    {
        .itvl_min = cfg->conn_itvl_units,
        .itvl_max = cfg->conn_itvl_units,
        .latency = 0,
        .supervision_timeout = BCB_SUPERVISION,
        .min_ce_len = cfg->ce_len_units / 2,
        .max_ce_len = cfg->ce_len_units,
    };

    rc = ble_gap_update_params(s_conn, &p);

    if (rc != 0)
    {
        ESP_LOGW(TAG, "connection update refused (%d)", rc);
    }

    if (cfg->phy_mask != BCB_PHY_1M)
    {
        rc = ble_gap_set_prefered_le_phy(s_conn, cfg->phy_mask, cfg->phy_mask, 0);

        if (rc != 0)
        {
            bcb_diag("PHY preference refused (rc %d)", rc);
        }
    }
}

/* ---- scanning + connecting ------------------------------------------------------------ */

static bool name_matches(const struct ble_hs_adv_fields *f, char *out, size_t cap)
{
    const char *target = bcb_settings_config()->target;

    if (f->name == NULL || f->name_len == 0)
    {
        return false;
    }

    size_t n = f->name_len < cap - 1 ? f->name_len : cap - 1;

    memcpy(out, f->name, n);
    out[n] = '\0';
    return strncmp(out, target, strlen(target)) == 0;
}

static uint32_t s_reports, s_named;
static int8_t   s_peer_rssi;

static void on_disc(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    char name[32];

    s_reports++;

    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0)
    {
        return;
    }

    if (f.name != NULL && f.name_len > 0)
    {
        s_named++;
    }

    /* the WiCAN's name rides in the SCAN RESPONSE (the legacy set carries
       flags + UUID only), so match on any report type and connect to that
       address: the peer is connectable regardless of which PDU we saw */
    if (!name_matches(&f, name, sizeof(name)))
    {
        return;
    }

    ble_gap_disc_cancel();
    strlcpy(s_peer_name, name, sizeof(s_peer_name));
    s_peer_rssi = d->rssi;

    char a[18];

    addr_str(&d->addr, a, sizeof(a));
    bcb_diag("found %s %s rssi %d (report type %u, %lu reports seen): connecting",
             name, a, d->rssi, d->event_type, (unsigned long)s_reports);

    int rc = ble_gap_connect(s_own_addr_type, &d->addr, 30000, NULL, gap_event, NULL);

    if (rc != 0)
    {
        bcb_diag("connect failed to start (rc %d)", rc);
        bcb_core_on_disconnected(rc);
    }
}

esp_err_t bcb_gap_connect(void)
{
    if (!s_synced || s_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_gap_disc_params p =
    {
        .itvl = 0x30, .window = 0x30, .filter_policy = 0,
        .limited = 0, .passive = 0, .filter_duplicates = 1,
    };

    s_reports = 0;
    s_named = 0;

    int rc = ble_gap_disc(s_own_addr_type, 30000, &p, gap_event, NULL);

    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        bcb_diag("scan failed to start (rc %d, own addr type %u)", rc, s_own_addr_type);
        return ESP_FAIL;
    }

    bcb_diag("scanning for '%s' (own addr type %u)", bcb_settings_config()->target, s_own_addr_type);
    return ESP_OK;
}

esp_err_t bcb_gap_disconnect(void)
{
    if (!s_connected)
    {
        (void)ble_gap_disc_cancel();
        (void)ble_gap_conn_cancel();
        return ESP_OK;
    }

    return ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM) == 0 ? ESP_OK : ESP_FAIL;
}

bool bcb_gap_connected(void)
{
    return s_connected;
}

uint16_t bcb_gap_mtu(void)
{
    return s_connected ? ble_att_mtu(s_conn) : 23;
}

/* ---- the GAP event handler ---------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_DISC:
            on_disc(&event->disc);
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (!s_connected && s_conn == BLE_HS_CONN_HANDLE_NONE)
            {
                bcb_diag("scan ended without the target (reason %d, %lu reports, %lu with a name)",
                         event->disc_complete.reason, (unsigned long)s_reports, (unsigned long)s_named);
                bcb_core_on_disconnected(event->disc_complete.reason);
            }
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                bcb_diag("connection failed (status %d)", event->connect.status);
                s_conn = BLE_HS_CONN_HANDLE_NONE;
                bcb_core_on_disconnected(event->connect.status);
                return 0;
            }

            {
                struct ble_gap_conn_desc d;
                char a[18] = "?";

                s_conn = event->connect.conn_handle;
                s_connected = true;

                if (ble_gap_conn_find(s_conn, &d) == 0)
                {
                    addr_str(&d.peer_ota_addr, a, sizeof(a));
                }

                bcb_diag("connected %s (%s)", s_peer_name, a);
                bcb_core_on_connected(s_peer_name, a, s_peer_rssi);
                tune_link();
                report_link();

                /* the peripheral no longer sends a Security Request: we start */
                int rc = ble_gap_security_initiate(s_conn);

                bcb_diag("security initiate rc %d", rc);
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            bcb_diag("disconnected (reason %d)", event->disconnect.reason);
            s_connected = false;
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_op_status = BLE_HS_ENOTCONN;
            xSemaphoreGive(s_op_done);
            bcb_core_on_disconnected(event->disconnect.reason);
            return 0;

        case BLE_GAP_EVENT_CONN_UPDATE_REQ:
            /* the peer (WiCAN asks 20-40 ms at connect): keep our interval */
            {
                const bcb_config_t *cfg = bcb_settings_config();

                event->conn_update_req.self_params->itvl_min = cfg->conn_itvl_units;
                event->conn_update_req.self_params->itvl_max = cfg->conn_itvl_units;
                event->conn_update_req.self_params->latency = 0;
                event->conn_update_req.self_params->supervision_timeout = BCB_SUPERVISION;
            }
            return 0;

        case BLE_GAP_EVENT_CONN_UPDATE:
            report_link();
            return 0;

        case BLE_GAP_EVENT_MTU:
            bcb_diag("MTU %u", event->mtu.value);
            report_link();
            return 0;

        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
            if (event->phy_updated.status == 0)
            {
                bcb_diag("PHY tx %u rx %u", event->phy_updated.tx_phy, event->phy_updated.rx_phy);
            }
            report_link();
            return 0;

        case BLE_GAP_EVENT_DATA_LEN_CHG:
            bcb_diag("DLE tx %u/%u us rx %u/%u us",
                     event->data_len_chg.max_tx_octets, event->data_len_chg.max_tx_time,
                     event->data_len_chg.max_rx_octets, event->data_len_chg.max_rx_time);
            bcb_core_on_link_params(0, 0, 0, 0, event->data_len_chg.max_tx_octets,
                                    event->data_len_chg.max_rx_octets);
            return 0;

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            {
                struct ble_sm_io io = { 0 };

                if (event->passkey.params.action == BLE_SM_IOACT_INPUT)
                {
                    io.action = BLE_SM_IOACT_INPUT;
                    io.passkey = bcb_settings_config()->passkey;
                }
                else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP)
                {
                    io.action = BLE_SM_IOACT_NUMCMP;
                    io.numcmp_accept = 1;
                }
                else
                {
                    return 0;
                }

                int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);

                bcb_diag("passkey action %d injected (rc %d)", event->passkey.params.action, rc);
            }
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            {
                struct ble_gap_conn_desc d;
                bool ok = event->enc_change.status == 0 && ble_gap_conn_find(s_conn, &d) == 0 &&
                          d.sec_state.encrypted && d.sec_state.authenticated;

                if (ok)
                {
                    bcb_diag("secured%s", d.sec_state.bonded ? " (bonded)" : "");
                    bcb_core_on_secured(true, d.sec_state.bonded);
                    bcb_disc_start(s_conn);
                }
                else
                {
                    bcb_diag("pairing failed (status 0x%03x)", event->enc_change.status);
                    bcb_core_on_secured(false, false);
                }
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            {
                /* a stale bond on our side: forget it and pair again */
                struct ble_gap_conn_desc d;

                if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) == 0)
                {
                    ble_store_util_delete_peer(&d.peer_id_addr);
                }
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        case BLE_GAP_EVENT_NOTIFY_RX:
            {
                uint8_t buf[512];
                uint16_t n = OS_MBUF_PKTLEN(event->notify_rx.om);

                if (n > sizeof(buf))
                {
                    n = sizeof(buf);
                }

                os_mbuf_copydata(event->notify_rx.om, 0, n, buf);
                bcb_core_on_notify(event->notify_rx.attr_handle, buf, n);
            }
            return 0;

        default:
            return 0;
    }
}

/* ---- blocking GATT primitives (bench task) ------------------------------------------- */

static int op_cb(uint16_t conn, const struct ble_gatt_error *err,
                 struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)arg;
    s_op_status = err->status;

    if (err->status == 0 && attr != NULL && s_op_dst != NULL)
    {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);

        if (n > s_op_cap)
        {
            n = (uint16_t)s_op_cap;
        }

        os_mbuf_copydata(attr->om, 0, n, s_op_dst);
        s_op_len = n;
    }

    xSemaphoreGive(s_op_done);
    return 0;
}

static esp_err_t op_wait(void)
{
    if (xSemaphoreTake(s_op_done, pdMS_TO_TICKS(BCB_OP_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    return s_op_status == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t bcb_gap_write(uint16_t handle, const uint8_t *data, size_t n)
{
    if (!s_connected)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_op_done, 0);
    s_op_dst = NULL;

    if (ble_gattc_write_flat(s_conn, handle, data, (uint16_t)n, op_cb, NULL) != 0)
    {
        return ESP_FAIL;
    }

    return op_wait();
}

int bcb_gap_read(uint16_t handle, uint8_t *dst, size_t cap)
{
    if (!s_connected)
    {
        return -1;
    }

    xSemaphoreTake(s_op_done, 0);
    s_op_dst = dst;
    s_op_cap = cap;
    s_op_len = 0;

    if (ble_gattc_read(s_conn, handle, op_cb, NULL) != 0)
    {
        return -1;
    }

    return op_wait() == ESP_OK ? (int)s_op_len : -1;
}

int bcb_gap_write_nr(uint16_t handle, const uint8_t *data, size_t n, uint32_t max_wait_ms)
{
    int retries = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(max_wait_ms);

    while (s_connected)
    {
        int rc = ble_gattc_write_no_rsp_flat(s_conn, handle, data, (uint16_t)n);

        if (rc == 0)
        {
            return retries;
        }

        if (rc != BLE_HS_ENOMEM || xTaskGetTickCount() > deadline)
        {
            return -1;
        }

        retries++;
        vTaskDelay(1); /* the reference's yield: let the controller drain */
    }

    return -1;
}

esp_err_t bcb_gap_subscribe(uint16_t cccd_handle, uint16_t value)
{
    uint8_t v[2] = { (uint8_t)value, 0 };

    return bcb_gap_write(cccd_handle, v, sizeof(v));
}

/* ---- host bring-up ----------------------------------------------------------------------- */

static void on_sync(void)
{
    const bcb_config_t *cfg = bcb_settings_config();

    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0)
    {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    (void)ble_att_set_preferred_mtu(cfg->mtu);
    (void)ble_gap_set_prefered_default_le_phy(cfg->phy_mask, cfg->phy_mask);
    s_synced = true;
    bcb_core_set_state(BCB_STATE_IDLE);
    bcb_diag("host synced (own addr type %u, mtu pref %u, phy mask 0x%02x)",
             s_own_addr_type, cfg->mtu, cfg->phy_mask);
}

static void on_reset(int reason)
{
    bcb_diag("host reset (%d)", reason);
    s_synced = false;
    s_connected = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t bcb_gap_start(void)
{
    s_op_done = xSemaphoreCreateBinary();

    if (s_op_done == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;   /* we type the passkey */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void bcb_gap_stop(void)
{
    if (nimble_port_stop() == 0)
    {
        nimble_port_deinit();
    }

    s_synced = false;
    s_connected = false;
}
