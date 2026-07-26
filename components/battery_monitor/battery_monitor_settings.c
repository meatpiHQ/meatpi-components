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
 * @file battery_monitor_settings.c
 * @brief settings_manager descriptor for battery_monitor: field-table schema
 *        (source of truth for shape/ranges/defaults) and on_apply (parses
 *        into the boot-applied config read through the batt_settings_*
 *        getters).
 */
#include "settings_manager.h"

#include "battery_monitor.h" /* battery_monitor_register_cli (settings-gated) */

#include "battery_monitor_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", true),
    SETTINGS_INT("poll_s", 1, 60, 3),
    SETTINGS_BOOL("cli", true),
};

/* boot-applied settings */
static bool s_enabled;
static uint32_t s_poll_ms;
static bool s_configured;

bool batt_settings_enabled(void)
{
    return s_enabled;
}

uint32_t batt_settings_poll_ms(void)
{
    return s_poll_ms;
}

bool batt_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings,
                                                         "enabled");

    s_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(settings, "poll_s");
    s_poll_ms = 1000u * (uint32_t)(cJSON_IsNumber(item) ? item->valueint
                                                        : 3);

    /* CLI ownership: settings-gated self-registration (reboot-to-apply) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && battery_monitor_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t batt_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "battery_monitor",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
