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
 * @file ble_manager_gatt_adv.c
 * @brief The advertising sets (NimBLE extended-advertising API, which is
 *        the only advertising API once CONFIG_BT_NIMBLE_EXT_ADV is on):
 *        instance 0 = the LEGACY PDU set every 4.2 scanner sees, with
 *        today's exact bytes (flags + tx power + the FFF0 128-bit UUID in
 *        the advertisement, name + "MeatPi" in the scan response);
 *        instance 1 = the BLE 5 EXTENDED set (connectable, one PDU with
 *        flags + UUID + name + mfg data, primary PHY 1M, secondary PHY per
 *        the `phy` setting). Which run is the `advertising` setting.
 */
#include <string.h>

#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"

#include "ble_manager.h"
#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

#define ADV_INST_LEGACY   0
#define ADV_INST_EXTENDED 1
#define ADV_ITVL          0x100   /* legacy 160 ms */

/* advertised service uuid: FFF0 on the 128-bit base (legacy) */
static const ble_uuid128_t ADV_SVC_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00);

static uint8_t s_mfg_data[] = "MeatPi";
static const char *s_name;
static uint8_t s_mode;
static bool s_configured[2];

static int set_data(uint8_t inst, const struct ble_hs_adv_fields *f, bool rsp)
{
    struct os_mbuf *om = os_msys_get_pkthdr(0, 0);

    if (om == NULL)
    {
        return BLE_HS_ENOMEM;
    }

    int rc = ble_hs_adv_set_fields_mbuf(f, om);

    if (rc != 0)
    {
        os_mbuf_free_chain(om);
        return rc;
    }

    /* the stack takes ownership of the mbuf */
    return rsp ? ble_gap_ext_adv_rsp_set_data(inst, om)
               : ble_gap_ext_adv_set_data(inst, om);
}

static int configure_legacy(uint8_t own_addr_type,
                            int (*gap_cb)(struct ble_gap_event *, void *))
{
    struct ble_gap_ext_adv_params p = { 0 };
    struct ble_hs_adv_fields adv = { 0 };
    struct ble_hs_adv_fields rsp = { 0 };

    /* IND = legacy_pdu + connectable + scannable (the 4.2 layout) */
    p.legacy_pdu = 1;
    p.connectable = 1;
    p.scannable = 1;
    p.own_addr_type = own_addr_type;
    p.primary_phy = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.itvl_min = ADV_ITVL;
    p.itvl_max = ADV_ITVL;
    p.sid = ADV_INST_LEGACY;
    p.tx_power = 127; /* no preference: the controller's setting */

    int rc = ble_gap_ext_adv_configure(ADV_INST_LEGACY, &p, NULL, gap_cb, NULL);

    if (rc != 0)
    {
        return rc;
    }

    /* adv: flags + txpower + the 128-bit FFF0-base uuid (legacy set;
       the full name rides the scan response, as on-air since 2026-07) */
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.tx_pwr_lvl_is_present = 1;
    /* the configured level, NOT BLE_HS_ADV_TX_PWR_LVL_AUTO: AUTO makes the
       host issue the legacy LE_Read_Advertising_Channel_Tx_Power command,
       which the controller answers "Command Disallowed" (0x0C) once it is
       driven through the extended-advertising API (bench, 2026-09-21) */
    adv.tx_pwr_lvl = (int8_t)blm_ident_clamp_tx_power(blm_core_config()->tx_power_dbm);
    adv.uuids128 = (ble_uuid128_t *)&ADV_SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;

    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rsp.mfg_data = s_mfg_data;
    rsp.mfg_data_len = sizeof(s_mfg_data) - 1;

    rc = set_data(ADV_INST_LEGACY, &adv, false);

    if (rc == 0)
    {
        rc = set_data(ADV_INST_LEGACY, &rsp, true);
    }

    return rc;
}

static int configure_extended(uint8_t own_addr_type, uint8_t phy_mask,
                              int (*gap_cb)(struct ble_gap_event *, void *))
{
    struct ble_gap_ext_adv_params p = { 0 };
    struct ble_hs_adv_fields adv = { 0 };

    /* extended: connectable, NOT scannable (the spec forbids both on an
       extended PDU); everything rides the one advertising payload */
    p.connectable = 1;
    p.own_addr_type = own_addr_type;
    p.primary_phy = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = (phy_mask & BLM_PHY_CODED) ? BLE_HCI_LE_PHY_CODED
                    : (phy_mask & BLM_PHY_2M) ? BLE_HCI_LE_PHY_2M
                                              : BLE_HCI_LE_PHY_1M;
    p.itvl_min = ADV_ITVL;
    p.itvl_max = ADV_ITVL;
    p.sid = ADV_INST_EXTENDED;
    p.tx_power = 127;

    int rc = ble_gap_ext_adv_configure(ADV_INST_EXTENDED, &p, NULL, gap_cb,
                                       NULL);

    if (rc != 0)
    {
        return rc;
    }

    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&ADV_SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    adv.name = (const uint8_t *)s_name;
    adv.name_len = (uint8_t)strlen(s_name);
    adv.name_is_complete = 1;
    adv.mfg_data = s_mfg_data;
    adv.mfg_data_len = sizeof(s_mfg_data) - 1;

    return set_data(ADV_INST_EXTENDED, &adv, false);
}

esp_err_t blm_adv_configure(uint8_t own_addr_type, const char *name,
                            uint8_t adv_mode, uint8_t phy_mask,
                            int (*gap_cb)(struct ble_gap_event *, void *))
{
    s_name = name;
    s_mode = adv_mode;

    int rc = 0;

    if (adv_mode != BLM_ADV_EXTENDED)
    {
        rc = configure_legacy(own_addr_type, gap_cb);
        s_configured[ADV_INST_LEGACY] = (rc == 0);

        if (rc != 0)
        {
            ESP_LOGE(TAG, "legacy advertising set config failed (%d)", rc);
            return ESP_FAIL;
        }
    }

    if (adv_mode != BLM_ADV_LEGACY)
    {
        rc = configure_extended(own_addr_type, phy_mask, gap_cb);
        s_configured[ADV_INST_EXTENDED] = (rc == 0);

        if (rc != 0)
        {
            /* the legacy set still works: degrade, never fail the stack */
            ESP_LOGW(TAG, "extended advertising set config failed (%d); "
                     "legacy set only", rc);
        }
    }

    return ESP_OK;
}

void blm_adv_start(void)
{
    bool any = false;

    for (uint8_t i = 0; i < 2; i++)
    {
        if (!s_configured[i])
        {
            continue;
        }

        int rc = ble_gap_ext_adv_start(i, 0, 0);

        if (rc != 0 && rc != BLE_HS_EALREADY)
        {
            ESP_LOGW(TAG, "advertising set %u start failed (%d)", i, rc);
            continue;
        }

        any = true;
    }

    if (any)
    {
        ESP_LOGI(TAG, "advertising (%s)",
                 s_mode == BLM_ADV_LEGACY ? "legacy" :
                 s_mode == BLM_ADV_EXTENDED ? "extended" : "legacy + extended");
    }
}

void blm_adv_stop(void)
{
    for (uint8_t i = 0; i < 2; i++)
    {
        if (s_configured[i])
        {
            (void)ble_gap_ext_adv_stop(i); /* EALREADY when idle: fine */
        }
    }
}
