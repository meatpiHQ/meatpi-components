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
 * @file restart_tracker_settings.c
 * @brief settings_manager descriptor for restart_tracker (CLI ownership).
 *
 * Minimal descriptor: the one knob is whether the component registers its
 * console command (`restart_tracker`) at boot. Init runs before
 * settings_manager_init, so main wires this separately (the
 * log_manager_register_settings pattern). Reboot-to-apply.
 */
#include "settings_manager.h"

#include "restart_tracker.h"

static const settings_field_t RT_SETTINGS_FIELDS[] =
{
    SETTINGS_BOOL("cli", true),
};

static esp_err_t rt_settings_apply(const cJSON *settings)
{
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && restart_tracker_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}

esp_err_t restart_tracker_register_settings(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "restart_tracker",
        .version     = 1,
        .fields      = RT_SETTINGS_FIELDS,
        .field_count = sizeof(RT_SETTINGS_FIELDS) /
                       sizeof(RT_SETTINGS_FIELDS[0]),
        .on_apply    = rt_settings_apply,
    };

    return settings_manager_register(&DESC);
}
