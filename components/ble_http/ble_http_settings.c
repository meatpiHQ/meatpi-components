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
 * @file ble_http_settings.c
 * @brief settings_manager descriptor for ble_http ("ble_http" v1):
 *        enabled + cli. Reboot-to-apply; on_apply fills the boot config and
 *        self-registers the `blehttp` console command (standard §6b).
 */
#include "settings_manager.h"

#include "ble_http.h"
#include "ble_http_private.h"

static const settings_field_t FIELDS[] =
{
    /* On by default: the channel only exists on a paired, MITM-
     * authenticated BLE link, and BLE itself ships off
     * (ble_manager.enabled=false). Off = the characteristics are not in
     * the GATT table at all, zero cost. */
    SETTINGS_BOOL("enabled", true),
    SETTINGS_BOOL("cli", true),
};

static bleh_config_t s_cfg = { .enabled = true };
static bool s_configured;

const bleh_config_t *bleh_settings_config(void)
{
    return &s_cfg;
}

bool bleh_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_cfg.enabled = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool reg;

        if (!reg && ble_http_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t bleh_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "ble_http",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
