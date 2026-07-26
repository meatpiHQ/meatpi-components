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
 * @brief The NimBLE GATT backend — the SAME on-air contract as
 *        ble_manager_gatt.c (Bluedroid), selected by the sdkconfig BT
 *        host choice (CONFIG_BT_NIMBLE_ENABLED) for the memory/perf
 *        A/B meatpi asked for (2026-07-05). Same UUIDs, Device Info
 *        strings, security level (SC+MITM+BOND, static passkey),
 *        MTU 517, adv layout and conn-window request.
 *
 *        Stack-mapping notes:
 *         - free-packets pacing: NimBLE exposes no controller-credit
 *           count; notify_* do a bounded ENOMEM retry instead and feed
 *           a congestion flag, which the IO layer's pacing tolerates.
 *         - CCCD security: NimBLE has no per-CCCD permission flags;
 *           notify_* additionally refuse until the link is secured
 *           (same effective policy as legacy's ENC_MITM CCCDs).
 *         - long CLI writes: NimBLE reassembles queued writes and
 *           delivers ONE access call — newline-split as usual.
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

/* ---- identity strings (byte-identical to the Bluedroid backend) --------------- */

static const char MANUFACTURER_NAME[] = "MEATPI.COM";
static const char MODEL_NUMBER[]      = "WiCAN-PRO";
static char s_serial_number[32]       = "";
static const char HARDWARE_REV[]      = "1_53         ";
static const char FIRMWARE_REV[]      = "400";
static const char SOFTWARE_REV[]      = "0000";
static const uint8_t SYSTEM_ID[8]     = { 0 };
static const uint8_t REG_CERT_DATA[8] = { 0 };

/* 128-bit CLI UUIDs — same LSB-first byte arrays as legacy */
static const ble_uuid128_t CLI_OUT_UUID = BLE_UUID128_INIT(
    0xBE, 0xF0, 0xAD, 0xDE, 0x34, 0x12, 0x78, 0x56,
    0x9A, 0xBC, 0xEF, 0x01, 0xC0, 0xDE, 0x00, 0x02);
static const ble_uuid128_t CLI_IN_UUID = BLE_UUID128_INIT(
    0xBE, 0xF0, 0xAD, 0xDE, 0x34, 0x12, 0x78, 0x56,
    0x9A, 0xBC, 0xEF, 0x01, 0xC0, 0xDE, 0x00, 0x03);

/* advertised service uuid: FFF0 on the 128-bit base (legacy) */
static const ble_uuid128_t ADV_SVC_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00);

/* ---- state ---------------------------------------------------------------------- */

static const char *s_dev_name;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_max_data = 20;
static volatile bool s_connected;
static volatile bool s_secured;
static volatile bool s_congested;
static uint8_t s_own_addr_type;

static uint16_t s_fff1_val_handle;
static uint16_t s_fff2_val_handle;
static uint16_t s_cli_out_val_handle;
static uint16_t s_cli_in_val_handle;

/* CLI IN reassembly (line splitting), PSRAM per §2 */
static char s_cli_buf[BLM_CLI_MAX] EXT_RAM_BSS_ATTR;
static size_t s_cli_len;

static void adv_start(void);

/* ---- CLI line reassembly (same semantics as the Bluedroid backend) ------------- */

static void cli_process_lines(void)
{
    size_t line_start = 0;

    for (size_t i = 0; i < s_cli_len; i++)
    {
        char c = s_cli_buf[i];

        if (c == '\r' || c == '\n')
        {
            s_cli_buf[i] = '\0';

            if (i > line_start)
            {
                blm_core_on_cli_line(&s_cli_buf[line_start]);
            }

            line_start = i + 1;
        }
    }

    if (line_start > 0)
    {
        size_t remaining = s_cli_len - line_start;

        memmove(s_cli_buf, &s_cli_buf[line_start], remaining);
        s_cli_len = remaining;
        s_cli_buf[s_cli_len] = '\0';
    }
}

/* ---- GATT access callbacks -------------------------------------------------------- */

/** Device Info: every characteristic is a static read-only string. */
static int dev_info_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const void *data = NULL;
    size_t len = 0;
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    /* lengths = sizeof() incl. the NUL, serial padded to 32 — the
       Bluedroid attribute table served exactly these bytes (legacy
       apps see identical values on either stack) */
    switch (uuid16)
    {
        case 0x2A29: data = MANUFACTURER_NAME; len = sizeof(MANUFACTURER_NAME); break;
        case 0x2A24: data = MODEL_NUMBER; len = sizeof(MODEL_NUMBER); break;
        case 0x2A25: data = s_serial_number; len = sizeof(s_serial_number); break;
        case 0x2A27: data = HARDWARE_REV; len = sizeof(HARDWARE_REV); break;
        case 0x2A26: data = FIRMWARE_REV; len = sizeof(FIRMWARE_REV); break;
        case 0x2A28: data = SOFTWARE_REV; len = sizeof(SOFTWARE_REV); break;
        case 0x2A23: data = SYSTEM_ID; len = sizeof(SYSTEM_ID); break;
        case 0x2A2A: data = REG_CERT_DATA; len = sizeof(REG_CERT_DATA); break;
        default: return BLE_ATT_ERR_UNLIKELY;
    }

    return (os_mbuf_append(ctxt->om, data, len) == 0)
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/** FFF0: FFF1/CLI OUT are notify-only (reads answer a dummy byte like
 *  the Bluedroid table); FFF2/CLI IN are the write pipes. */
static int fff0_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        static const uint8_t dummy = 0x00;

        return (os_mbuf_append(ctxt->om, &dummy, 1) == 0)
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

    if (attr_handle == s_fff2_val_handle)
    {
        /* the data pipe — the GATT flags (WRITE_ENC|WRITE_AUTHEN) already
           bar unpaired writes; this mirrors the CLI path's software check
           so the vehicle-command pipe is gated symmetrically */
        if (!s_secured)
        {
            ESP_LOGW(TAG, "data write rejected: link not securely paired");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        /* copy out flat and fan out (hot path) */
        static uint8_t rx_buf[BLM_SEND_BUF_SIZE] EXT_RAM_BSS_ATTR;
        uint16_t out_len = 0;

        if (ble_hs_mbuf_to_flat(ctxt->om, rx_buf, sizeof(rx_buf),
                                &out_len) != 0)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        blm_core_on_rx(rx_buf, out_len);
        return 0;
    }

    if (attr_handle == s_cli_in_val_handle)
    {
        if (!s_secured)
        {
            ESP_LOGW(TAG, "CLI write rejected: link not securely paired");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        size_t room = BLM_CLI_MAX - 1 - s_cli_len;
        uint16_t to_copy = (len > room) ? (uint16_t)room : len;
        uint16_t copied = 0;

        ble_hs_mbuf_to_flat(ctxt->om, (uint8_t *)&s_cli_buf[s_cli_len],
                            to_copy, &copied);
        s_cli_len += copied;
        s_cli_buf[s_cli_len] = '\0';
        cli_process_lines();
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ---- service tables ----------------------------------------------------------------- */

/* Device Info reads require an encrypted + authenticated (paired/bonded)
   link too (meatpi 2026-07-08): NO characteristic is readable by an
   unpaired peer — an unpaired read gets Insufficient Authentication,
   which prompts the client to pair. */
#define DI_CHR(uuid16) \
    { .uuid = BLE_UUID16_DECLARE(uuid16), .access_cb = dev_info_access, \
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | \
               BLE_GATT_CHR_F_READ_AUTHEN }

static const struct ble_gatt_svc_def GATT_SVCS[] =
{
    {   /* Device Information (0x180A) — same eight characteristics */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[])
        {
            DI_CHR(0x2A29), DI_CHR(0x2A24), DI_CHR(0x2A25), DI_CHR(0x2A27),
            DI_CHR(0x2A26), DI_CHR(0x2A28), DI_CHR(0x2A23), DI_CHR(0x2A2A),
            { 0 }
        },
    },
    {   /* FFF0 — the data pipes + the CLI pipes */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFF0),
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {   /* FFF1: data OUT (notify/indicate; enc+authen reads) */
                .uuid = BLE_UUID16_DECLARE(0xFFF1),
                .access_cb = fff0_access,
                .val_handle = &s_fff1_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
            },
            {   /* FFF2: data IN (write/write-nr; enc+authen) */
                .uuid = BLE_UUID16_DECLARE(0xFFF2),
                .access_cb = fff0_access,
                .val_handle = &s_fff2_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {   /* CLI OUT (128-bit) */
                .uuid = &CLI_OUT_UUID.u,
                .access_cb = fff0_access,
                .val_handle = &s_cli_out_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
            },
            {   /* CLI IN (128-bit) */
                .uuid = &CLI_IN_UUID.u,
                .access_cb = fff0_access,
                .val_handle = &s_cli_in_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ---- GAP ------------------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                adv_start();
                break;
            }

            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_secured = false;
            s_cli_len = 0;

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

            /* NOTE: do NOT request Data Length Extension here. Tried
               2026-07-05 (the esp-idf ble_throughput demo's lever):
               with WiFi coex active the 251-byte/2.1 ms LL packets
               collide with WiFi airtime and TX COLLAPSED 28 -> 2 KB/s,
               plus connect instability. The demo's numbers assume a
               radio without WiFi. Controllers may still negotiate DLE
               on their own — just don't force long TX packets. */

            if (blm_core_pairing_allowed())
            {
                /* mirror of legacy esp_ble_set_encryption on connect */
                ble_gap_security_initiate(s_conn_handle);
            }

            blm_core_on_connect();
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_secured = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_cli_len = 0;
            blm_core_on_disconnect();
            adv_start(); /* re-advertise */
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
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

            ESP_LOGI(TAG, "pairing %s", s_secured ? "success" : "failed");
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

/* ---- advertising ----------------------------------------------------------------------- */

static void adv_start(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_hs_adv_fields rsp = { 0 };
    struct ble_gap_adv_params params = { 0 };
    static uint8_t mfg_data[] = "MeatPi";

    /* adv: flags + txpower + the 128-bit FFF0-base uuid (legacy set;
       the full name rides the scan response, as on-air with Bluedroid) */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = (ble_uuid128_t *)&ADV_SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rsp.name = (const uint8_t *)s_dev_name;
    rsp.name_len = strlen(s_dev_name);
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfg_data;
    rsp.mfg_data_len = sizeof(mfg_data) - 1;

    if (ble_gap_adv_set_fields(&fields) != 0 ||
        ble_gap_adv_rsp_set_fields(&rsp) != 0)
    {
        ESP_LOGE(TAG, "adv field config failed");
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 0x100; /* legacy 160 ms */
    params.itvl_max = 0x100;

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, gap_event, NULL);

    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGE(TAG, "advertising start failed (%d)", rc);
    }
    else
    {
        ESP_LOGI(TAG, "advertising");
    }
}

static void on_sync(void)
{
    /* resolvable private address like legacy; public as fallback */
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(1, &s_own_addr_type) != 0)
    {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    adv_start();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (%d)", reason);
    s_connected = false;
    s_secured = false;
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
    blm_ident_serial(dev_name, s_serial_number, sizeof(s_serial_number));

    esp_err_t err = nimble_port_init(); /* controller + host */

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NimBLE bring-up failed: %s", esp_err_to_name(err));
        return err;
    }

    /* security block: LE Secure Connections + MITM (static passkey via
       DisplayOnly). Bonding (long-term key exchange/retention) is a
       setting — OFF still encrypts+authenticates per session but keeps no
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

    int rc = ble_gatts_count_cfg(GATT_SVCS);

    if (rc == 0)
    {
        rc = ble_gatts_add_svcs(GATT_SVCS);
    }

    if (rc == 0)
    {
        rc = ble_svc_gap_device_name_set(dev_name);
    }

    if (rc != 0)
    {
        ESP_LOGE(TAG, "GATT registration failed (%d)", rc);
        return ESP_FAIL;
    }

    ble_att_set_preferred_mtu(517); /* legacy */

    /* TX power (settings, clamped) — controller API, stack-agnostic */
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

uint16_t blm_gatt_max_data(void)
{
    return s_max_data;
}

bool blm_gatt_congested(void)
{
    return s_congested;
}

int blm_gatt_free_packets(void)
{
    /* NimBLE exposes no controller-credit count: report a fixed credit
       while healthy; ENOMEM inside notify_* flips the congestion flag,
       which the IO layer's pacing loop honors. */
    return (s_connected && !s_congested) ? 8 : 0;
}

/** Notify with a bounded ENOMEM retry (the pacing emulation). */
static esp_err_t notify_handle(uint16_t val_handle, const uint8_t *buf,
                               uint16_t len)
{
    if (!s_connected || !s_secured)
    {
        return ESP_ERR_INVALID_STATE; /* legacy CCCD-security equivalent */
    }

    for (int attempt = 0; attempt < 50; attempt++)
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);

        if (om == NULL)
        {
            s_congested = true;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);

        if (rc == 0)
        {
            s_congested = false;
            return ESP_OK;
        }

        if (rc != BLE_HS_ENOMEM)
        {
            return ESP_FAIL;
        }

        s_congested = true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t blm_gatt_notify_data(const uint8_t *buf, uint16_t len)
{
    return notify_handle(s_fff1_val_handle, buf, len);
}

esp_err_t blm_gatt_notify_cli(const uint8_t *buf, uint16_t len)
{
    return notify_handle(s_cli_out_val_handle, buf, len);
}

void blm_gatt_allow_pairing(bool allow)
{
    /* legacy: enabling pairing mid-connection kicks off encryption */
    if (allow && s_connected && !s_secured)
    {
        ble_gap_security_initiate(s_conn_handle);
    }
}
