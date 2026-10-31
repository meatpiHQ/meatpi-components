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
 * @file ble_manager_ctrl.c
 * @brief The Bluetooth memory profile applied onto the controller config
 *        (settings v5, 2026-10-31). The ESP32-S3 controller sizes itself
 *        from esp_bt_controller_config_t at esp_bt_controller_init(): the
 *        activity slots (828 B each), the static ACL TX buffers, the scan
 *        duplicate caches, the BLE 5 feature set. BT_CONTROLLER_INIT_CONFIG
 *        _DEFAULT() carries the build's Kconfig values; the Full profile
 *        keeps them, Lean and Custom overwrite the four the profile owns.
 */
#include "ble_manager_ctrl.h"

#include "esp_log.h"

static const char *TAG = "ble_manager";

static blm_ram_cfg_t s_applied;
static bool s_applied_valid;

void blm_ctrl_configure(esp_bt_controller_config_t *bt_cfg)
{
    const blm_config_t *cfg = blm_core_config();
    const blm_ram_cfg_t full =
    {
        .max_act = bt_cfg->ble_max_act,
        .acl_tx_bufs = bt_cfg->ble_st_acl_tx_buf_nb,
        .scan_cache = bt_cfg->normal_adv_size,
        .ble5 = bt_cfg->ble_50_feat_supp,
    };

    blm_ident_ram_config(cfg->ram_profile, &full, &cfg->ram_custom, &s_applied);
    s_applied_valid = true;

    bt_cfg->ble_max_act = s_applied.max_act;
    bt_cfg->ble_st_acl_tx_buf_nb = s_applied.acl_tx_bufs;
    /* ble_50_feat_supp stays the build's: the mapping never changes it */

    if (s_applied.scan_cache == 0)
    {
        /* this device never scans (no central role in the product): the
           scan feature and both duplicate caches off */
        bt_cfg->scan_en = false;
        bt_cfg->scan_duplicate_mode = 0;
        bt_cfg->normal_adv_size = 0;
        bt_cfg->mesh_adv_size = 0;
        bt_cfg->ble_adv_dup_filt_max = 0;
    }
    else
    {
        bt_cfg->normal_adv_size = s_applied.scan_cache;
    }

    ESP_LOGI(TAG, "ble ram profile=%s (act=%u acl_tx=%u scan=%u ble5=%d)",
             blm_ident_ram_profile_word(cfg->ram_profile), s_applied.max_act,
             s_applied.acl_tx_bufs, s_applied.scan_cache, s_applied.ble5 ? 1 : 0);
}

const blm_ram_cfg_t *blm_ctrl_applied(void)
{
    return s_applied_valid ? &s_applied : NULL;
}
