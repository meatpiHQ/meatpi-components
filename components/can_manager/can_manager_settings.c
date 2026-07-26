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
 * @file can_manager_settings.c
 * @brief settings_manager descriptor for can_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_apply (stores the
 *        boot-applied bus knobs + the settings-gated CLI registration §6b).
 */
#include <stdlib.h>

#include "settings_manager.h"

#include "can_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_STR_ENUM("baud", "33,83,95,100,125,250,500,1000", "500"),
    SETTINGS_BOOL("silent", false),
    SETTINGS_BOOL("cli", true),
};

/* boot-applied settings */
static bool s_enabled;
static bool s_silent;
static uint32_t s_baud_kbps = 500;
static bool s_configured;

bool canm_settings_enabled(void)
{
    return s_enabled;
}

bool canm_settings_silent(void)
{
    return s_silent;
}

uint32_t canm_settings_baud_kbps(void)
{
    return s_baud_kbps;
}

bool canm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    s_silent = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "silent"));

    const cJSON *baud = cJSON_GetObjectItemCaseSensitive(settings, "baud");

    s_baud_kbps = 500;

    if (cJSON_IsString(baud) && baud->valuestring != NULL)
    {
        int v = atoi(baud->valuestring);

        if (v > 0)
        {
            s_baud_kbps = (uint32_t)v;
        }
    }

    /* CLI ownership: settings-gated self-registration (§6b) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && can_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t canm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "can_manager",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
