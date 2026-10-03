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
 * @file j1939_settings.c
 * @brief settings_manager descriptor of the J1939 component: the field table
 *        (`enabled`, `mode`, `address`, `cli`), on_apply (the boot-applied
 *        values, the console command) and the getters. Reboot-to-apply
 *        (Standard 4.2). `mode` and `address` were added 2026-10-03 (phase
 *        6) as non-breaking additions: a stored v1 file without them gets
 *        the defaults at boot (settings_manager fills missing keys).
 */
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "settings_manager.h"

#include "j1939_claim_core.h"
#include "j1939_private.h"

static const char *TAG = "j1939";

/* clang-format off */
static const settings_field_t FIELDS[] =
{
    /* off by default: it needs the native CAN bus (can_manager), which is
       off by default too */
    SETTINGS_BOOL("enabled", false),
    /* listen: never a frame on the bus. active: claim an address, answer
       requests, ask for groups, clear codes (needs can_manager.silent off) */
    SETTINGS_STR_ENUM("mode", "listen,active", "listen"),
    /* the address claimed first: 249 = off-board diagnostic service tool 1
       (J1939_CLAIM_TOOL_1); lost to a better NAME the node moves to 250,
       then 128..247 (J1939_CLAIM_ADDR_MIN..MAX). Literals: the UI preview's
       schema extractor reads this table */
    SETTINGS_INT("address", 128, 253, 249),
    SETTINGS_BOOL("cli",     true),
};
/* clang-format on */

static bool s_configured;
static bool s_enabled;
static bool s_active;
static uint8_t s_address = J1939_CLAIM_TOOL_1;

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(settings, "mode");
    const cJSON *addr = cJSON_GetObjectItemCaseSensitive(settings, "address");

    s_enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings,
                                                             "enabled"));
    s_active = cJSON_IsString(mode) && strcmp(mode->valuestring, "active") == 0;
    s_address = (cJSON_IsNumber(addr) && addr->valueint >= J1939_CLAIM_ADDR_MIN &&
                 addr->valueint <= 253)
                    ? (uint8_t)addr->valueint
                    : J1939_CLAIM_TOOL_1;
    s_configured = true;

    /* CLI ownership: settings-gated self-registration (Standard §6b) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered)
        {
            esp_err_t err = j1939_register_cli();

            if (err == ESP_OK)
            {
                s_cli_registered = true;
            }
            else
            {
                ESP_LOGW(TAG, "console command not registered (%s)",
                         esp_err_to_name(err));
            }
        }
    }

    return ESP_OK;
}

esp_err_t j1939_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "j1939",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}

bool j1939_settings_is_configured(void)
{
    return s_configured;
}

bool j1939_settings_enabled(void)
{
    return s_enabled;
}

bool j1939_settings_active(void)
{
    return s_active;
}

uint8_t j1939_settings_address(void)
{
    return s_address;
}
