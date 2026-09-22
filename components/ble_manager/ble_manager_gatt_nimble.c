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
 * @file ble_manager_gatt_nimble.c
 * @brief The NimBLE GATT backend - the SAME on-air contract as
 *        ble_manager_gatt.c (Bluedroid), selected by the sdkconfig BT
 *        host choice (CONFIG_BT_NIMBLE_ENABLED) for the memory/perf
 *        A/B meatpi asked for (2026-07-05). Same UUIDs, Device Info
 *        strings, security level (SC+MITM+BOND, static passkey),
 *        MTU 517, adv layout and conn-window request. The service
 *        table + access callbacks live in ble_manager_gatt_svc.c; this
 *        file owns GAP, security, advertising and the notify path.
 *
 *        Stack-mapping notes:
 *         - free-packets pacing: NimBLE exposes no controller-credit
 *           count; notify_* do a bounded ENOMEM retry instead and feed
 *           a SELF-EXPIRING congestion window (ble_manager_gatt_tx.c), which the IO layer's
 *           pacing tolerates. (The 2026-09-21 slcan bench found the
 *           former latch never cleared once the TX task stopped
 *           notifying while it was set: BLE TX dead for the boot.)
 *         - CCCD security: NimBLE has no per-CCCD permission flags;
 *           notify_* additionally refuse until the link is secured
 *           (same effective policy as legacy's ENC_MITM CCCDs).
 *         - long CLI writes: NimBLE reassembles queued writes and
 *           delivers ONE access call - newline-split as usual.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_manager.h"
#include "ble_manager_private.h"

/* NimBLE's NVS-backed key store: the config store module registers the
   read/write/delete callbacks (no public header in the IDF include path) */
extern void ble_store_config_init(void);

static const char *TAG = "ble_manager";

/* ---- state ---------------------------------------------------------------------- */

static const char *s_dev_name;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_max_data = 20;
static volatile bool s_connected;
static volatile bool s_secured;
static uint8_t s_own_addr_type;

/* the live link's PHYs (BLE_HCI_LE_PHY_1M/2M/CODED = 1/2/3), 0 = none */
static volatile uint8_t s_phy_tx;
static volatile uint8_t s_phy_rx;

static bool s_phy_requested;           /* one PHY request per link */

/* the `phy` setting: ask for the preferred PHY(s); the central decides (a
   4.2 peer keeps 1M), the result comes back as PHY_UPDATE_COMPLETE */
static void phy_request(void)
{
    uint8_t mask = blm_core_config()->phy_mask;

    if (mask == BLM_PHY_1M || s_phy_requested || !s_connected)
    {
        return;
    }

    if ((mask == BLM_PHY_2M && s_phy_tx == BLE_HCI_LE_PHY_2M) ||
        (mask == BLM_PHY_CODED && s_phy_tx == BLE_HCI_LE_PHY_CODED))
    {
        s_phy_requested = true; /* the controller's default preference already did it */
        return;
    }

    s_phy_requested = true;

    int prc = ble_gap_set_prefered_le_phy(s_conn_handle, mask, mask,
                                          BLE_GAP_LE_PHY_CODED_ANY);

    if (prc != 0)
    {
        ESP_LOGW(TAG, "PHY preference 0x%02x refused (%d)", mask, prc);
    }
}

/* ---- GAP ------------------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                blm_adv_start();
                break;
            }

            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_secured = false;
            s_phy_tx = BLE_HCI_LE_PHY_1M; /* every link starts on 1M */
            s_phy_rx = BLE_HCI_LE_PHY_1M;
            blm_tx_on_link_reset();
            blm_svc_reset_rx();
            blm_adv_stop(); /* one central: the other advertising set too */

            {
                /* the configured connection window (conn_profile) */
                struct ble_gap_upd_params params =
                {
                    .itvl_min = blm_core_config()->conn_min_units,
                    .itvl_max = blm_core_config()->conn_max_units,
                    .latency = 0,
                    .supervision_timeout = 400, /* 4 s (legacy) */
                };

                ble_gap_update_params(s_conn_handle, &params);
            }

            /* the `phy` setting is requested in CONN_UPDATE / ENC_CHANGE
               (phy_request), once the 4 s supervision timeout above is in
               force: asked here, at connect, the 2M switch happened under
               the central's default 420 ms timeout and the link dropped
               with reason 0x08 every time (UB500/BlueZ, 2026-09-21) */
            s_phy_requested = false;

            /* NOTE: do NOT request Data Length Extension here. Tried
               2026-07-05 (the esp-idf ble_throughput demo's lever):
               with WiFi coex active the 251-byte/2.1 ms LL packets
               collide with WiFi airtime and TX COLLAPSED 28 -> 2 KB/s,
               plus connect instability. The demo's numbers assume a
               radio without WiFi. Controllers may still negotiate DLE
               on their own - just don't force long TX packets. */

            /* No Security Request from our side at connect (the legacy
               esp_ble_set_encryption mirror, removed 2026-09-21): phones
               pair on their first encrypted access anyway, and a
               peripheral-initiated request makes Windows start a
               system pairing that an app's own ceremony then cannot join
               (WinRT custom pairing "Failed", no ceremony asked; the DUT
               saw ENOTCONN 2 s in). The runtime pairing window still
               applies through PASSKEY_ACTION. */

            blm_core_on_connect();
            blm_channel_on_link(BLE_MANAGER_CH_CONNECTED);
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_secured = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_phy_tx = 0;
            s_phy_rx = 0;
            blm_tx_on_link_reset(); /* + wakes an indication waiter */
            blm_svc_reset_rx();
            blm_core_on_disconnect();
            blm_channel_on_link(BLE_MANAGER_CH_DISCONNECTED);
            blm_adv_start(); /* re-advertise the configured set(s) */
            break;

        case BLE_GAP_EVENT_CONN_UPDATE:
            if (event->conn_update.status == 0)
            {
                phy_request(); /* our 4 s supervision timeout is live now */
            }
            break;

        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
            if (event->phy_updated.status == 0)
            {
                s_phy_tx = event->phy_updated.tx_phy;
                s_phy_rx = event->phy_updated.rx_phy;
                ESP_LOGI(TAG, "PHY now tx %s rx %s",
                         s_phy_tx == BLE_HCI_LE_PHY_2M ? "2M" :
                         s_phy_tx == BLE_HCI_LE_PHY_CODED ? "coded" : "1M",
                         s_phy_rx == BLE_HCI_LE_PHY_2M ? "2M" :
                         s_phy_rx == BLE_HCI_LE_PHY_CODED ? "coded" : "1M");
            }
            else
            {
                ESP_LOGW(TAG, "PHY update failed (%d); staying on the current PHY",
                         event->phy_updated.status);
            }
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            /* an advertising instance stopped (the one that produced the
               connection, or our own stop); DISCONNECT restarts the set(s) */
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            /* the central wrote a CCCD (or a bonded one's was restored):
               a stream channel's OUT mode follows it */
            blm_channel_on_subscribe(event->subscribe.attr_handle,
                                     event->subscribe.cur_notify,
                                     event->subscribe.cur_indicate);
            break;

        case BLE_GAP_EVENT_NOTIFY_TX:
            /* status 0 = the PDU went to the controller; for an indication
               EDONE = the central confirmed it, ETIMEOUT = it did not */
            if (event->notify_tx.indication &&
                event->notify_tx.status != 0)
            {
                blm_tx_on_indicate_done(event->notify_tx.status);
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            phy_request(); /* fallback when the central kept its own parameters */
        {
            struct ble_gap_conn_desc desc;

            if (event->enc_change.status == 0 &&
                ble_gap_conn_find(event->enc_change.conn_handle,
                                  &desc) == 0)
            {
                s_secured = desc.sec_state.encrypted &&
                            desc.sec_state.authenticated;
                ESP_LOGD(TAG, "sec: enc=%d authen=%d bonded=%d keysz=%d",
                         desc.sec_state.encrypted,
                         desc.sec_state.authenticated,
                         desc.sec_state.bonded, desc.sec_state.key_size);
            }

            if (s_secured)
            {
                ESP_LOGI(TAG, "pairing success");
            }
            else
            {
                /* NimBLE status: BLE_HS_SM_US_ERR(x) = 0x500+x our side,
                   BLE_HS_SM_PEER_ERR(x) = 0x600+x the peer's SMP reason */
                ESP_LOGW(TAG, "pairing failed (status 0x%03x)", event->enc_change.status);
            }

            if (s_secured)
            {
                blm_channel_on_link(BLE_MANAGER_CH_SECURED);
            }
            break;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            if (!blm_core_pairing_allowed())
            {
                /* the runtime pairing window (legacy SEC_REQ veto) */
                ESP_LOGW(TAG, "pairing refused (window closed)");
                return BLE_HS_EREJECT;
            }

            if (event->passkey.params.action == BLE_SM_IOACT_DISP)
            {
                /* the static passkey is "displayed" (never logged) */
                struct ble_sm_io io =
                {
                    .action = BLE_SM_IOACT_DISP,
                    .passkey = blm_core_config()->passkey,
                };

                ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            break;

        case BLE_GAP_EVENT_MTU:
            s_mtu = event->mtu.value;
            s_max_data = (BLM_SEND_BUF_SIZE <= s_mtu - 3)
                             ? BLM_SEND_BUF_SIZE : (uint16_t)(s_mtu - 3);
            ESP_LOGI(TAG, "MTU %u (payload %u)", s_mtu, s_max_data);
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            /* peer lost its bond: delete ours and re-pair (bench-friendly;
               the stale-bond disconnect gotcha from the Pi bench) */
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->repeat_pairing.conn_handle,
                                  &desc) == 0)
            {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }

            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        default:
            break;
    }

    return 0;
}

/* ---- host sync: address, PHY preference, advertising sets ------------------------------- */

static void on_sync(void)
{
    const blm_config_t *cfg = blm_core_config();

    /* attribute handles are assigned by now (ble_gatts_start ran) */
    blm_svc_publish_handles();

    /* resolvable private address like legacy; public as fallback */
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(1, &s_own_addr_type) != 0)
    {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    /* the `phy` setting as the controller's default preference for new
       links: it is what our LL answers a central's PHY request with, so
       `1m` (mask 0x01) really keeps a 5.0 central on 1M (a PC's Intel
       adapter asked for 2M on its own and got it while the preference was
       unset, 2026-09-21), and `2m`/`coded`/`auto` make the controller
       start the switch itself right after the connection */
    {
        int prc = ble_gap_set_prefered_default_le_phy(cfg->phy_mask,
                                                      cfg->phy_mask);

        if (prc != 0)
        {
            ESP_LOGW(TAG, "default PHY preference 0x%02x refused (%d)",
                     cfg->phy_mask, prc);
        }
    }

    /* the advertising set(s) per the `advertising` setting (the adv file
       owns the on-air bytes; the legacy set is byte-identical to 2026-07) */
    if (blm_adv_configure(s_own_addr_type, s_dev_name, cfg->adv_mode,
                          cfg->phy_mask, gap_event) == ESP_OK)
    {
        blm_adv_start();
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (%d)", reason);
    s_connected = false;
    s_secured = false;
    blm_tx_on_link_reset();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();          /* returns at nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ---- stack lifecycle ---------------------------------------------------------------------- */

esp_err_t blm_gatt_stack_up(const char *dev_name)
{
    const blm_config_t *cfg = blm_core_config();

    s_dev_name = dev_name;

    blm_tx_init(); /* the indication semaphores, once */

    esp_err_t err = nimble_port_init(); /* controller + host */

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NimBLE bring-up failed: %s", esp_err_to_name(err));
        return err;
    }

    /* security block: LE Secure Connections + MITM (static passkey via
       DisplayOnly). Bonding (long-term key exchange/retention) is a
       setting - OFF still encrypts+authenticates per session but keeps no
       reusable key, so every reconnect re-pairs. */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = cfg->bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    /* refuse the legacy-pairing downgrade unless a pre-4.2 accessory
       needs it (v3, default on) */
    ble_hs_cfg.sm_sc_only = cfg->sc_only ? 1 : 0;
    /* only exchange the long-term enc + identity keys when bonding; with
       bonding off there is nothing reusable to distribute or store */
    ble_hs_cfg.sm_our_key_dist = cfg->bonding
        ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;
    ble_hs_cfg.sm_their_key_dist = cfg->bonding
        ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;

    /* persistent bond store (CONFIG_BT_NIMBLE_NVS_PERSIST): keeps the
       long-term keys in NVS so a bonded phone reconnects WITHOUT a fresh
       passkey, across reboots/power-cycles. Round-robin overflow evicts
       the oldest bond once MAX_BONDS (3) is reached instead of failing. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    err = blm_svc_register(dev_name);

    if (err != ESP_OK)
    {
        return err;
    }

    ble_att_set_preferred_mtu(517); /* legacy */

    /* TX power (settings, clamped) - controller API, stack-agnostic */
    esp_power_level_t lvl;

    switch (cfg->tx_power_dbm)
    {
        case -12: lvl = ESP_PWR_LVL_N12; break;
        case -9:  lvl = ESP_PWR_LVL_N9;  break;
        case -6:  lvl = ESP_PWR_LVL_N6;  break;
        case -3:  lvl = ESP_PWR_LVL_N3;  break;
        case 0:   lvl = ESP_PWR_LVL_N0;  break;
        case 3:   lvl = ESP_PWR_LVL_P3;  break;
        case 6:   lvl = ESP_PWR_LVL_P6;  break;
        default:  lvl = ESP_PWR_LVL_P9;  break;
    }

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, lvl);

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void blm_gatt_stack_down(void)
{
    if (nimble_port_stop() == 0)
    {
        nimble_port_deinit();
    }

    s_connected = false;
    s_secured = false;
    blm_tx_on_link_reset();
}

/* ---- accessors for the IO layer -------------------------------------------------------------- */

bool blm_gatt_connected(void)
{
    return s_connected;
}

bool blm_gatt_secured(void)
{
    return s_secured;
}

uint16_t blm_gatt_conn_handle(void)
{
    return s_conn_handle;
}

uint16_t blm_gatt_max_data(void)
{
    return s_max_data;
}

uint16_t blm_gatt_channel_out_handle(int idx)
{
    return blm_svc_channel_out_handle(idx);
}

void blm_gatt_phy(uint8_t *tx, uint8_t *rx)
{
    uint8_t t = 0;
    uint8_t r = 0;

    if (s_connected)
    {
        /* the controller's answer wins over the cached event values */
        if (ble_gap_read_le_phy(s_conn_handle, &t, &r) != 0)
        {
            t = s_phy_tx;
            r = s_phy_rx;
        }
    }

    if (tx != NULL) *tx = t;
    if (rx != NULL) *rx = r;
}

void blm_gatt_allow_pairing(bool allow)
{
    /* legacy: enabling pairing mid-connection kicks off encryption */
    if (allow && s_connected && !s_secured)
    {
        ble_gap_security_initiate(s_conn_handle);
    }
}
