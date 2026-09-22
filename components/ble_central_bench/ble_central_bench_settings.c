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
 * @file ble_central_bench_settings.c
 * @brief settings_manager descriptor "ble_central_bench" v1: the link
 *        tuning of esp-idf's throughput_app as settings (MTU 517, 7.5 ms
 *        interval, DLE, PHY), the target name and passkey. Reboot-to-apply;
 *        on_apply fills the boot config and self-registers the CLI (§6b).
 */
#include <string.h>

#include "settings_manager.h"

#include "ble_central_bench_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL    ("enabled", false),          /* a bench role: opt-in     */
    SETTINGS_STR     ("target", 31, "WiC_"),      /* name prefix or full name */
    SETTINGS_INT     ("passkey", 0, 999999, 123456),
    SETTINGS_INT     ("mtu", 23, 517, 517),
    SETTINGS_INT     ("conn_itvl_units", 6, 3200, 6),   /* 1.25 ms units    */
    SETTINGS_INT     ("ce_len_units", 0, 0xFFFF, 24),   /* 0.625 ms units   */
    SETTINGS_BOOL    ("dle", true),
    SETTINGS_STR_ENUM("phy", "1m,2m", "1m"),
    SETTINGS_BOOL    ("cli", true),
};

static bcb_config_t s_cfg =
{
    .enabled = false, .target = "WiC_", .passkey = 123456, .mtu = 517,
    .conn_itvl_units = 6, .ce_len_units = 24, .dle = true,
    .phy_mask = BCB_PHY_1M, .cli = true,
};
static bool s_configured;

const bcb_config_t *bcb_settings_config(void)
{
    return &s_cfg;
}

bool bcb_settings_is_configured(void)
{
    return s_configured;
}

static int get_int(const cJSON *s, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, k);

    return cJSON_IsNumber(v) ? v->valueint : def;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *v;

    s_cfg.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "enabled"));

    v = cJSON_GetObjectItemCaseSensitive(settings, "target");

    if (cJSON_IsString(v) && v->valuestring[0] != '\0')
    {
        strlcpy(s_cfg.target, v->valuestring, sizeof(s_cfg.target));
    }

    s_cfg.passkey = (uint32_t)get_int(settings, "passkey", 123456);
    s_cfg.mtu = (uint16_t)get_int(settings, "mtu", 517);
    s_cfg.conn_itvl_units = (uint16_t)get_int(settings, "conn_itvl_units", 6);
    s_cfg.ce_len_units = (uint16_t)get_int(settings, "ce_len_units", 24);
    s_cfg.dle = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "dle"));

    v = cJSON_GetObjectItemCaseSensitive(settings, "phy");
    s_cfg.phy_mask = (cJSON_IsString(v) && strcmp(v->valuestring, "2m") == 0)
                     ? BCB_PHY_2M : BCB_PHY_1M;

    s_cfg.cli = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli"));

    if (s_cfg.cli)
    {
        static bool reg;

        if (!reg && ble_central_bench_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t bcb_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "ble_central_bench",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
