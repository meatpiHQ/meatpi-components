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
 * @file filesystem_settings.c
 * @brief settings_manager descriptor for filesystem (CLI ownership).
 *
 * Minimal descriptor: the one knob is whether the component registers its
 * console command (`fs`) at boot. Init runs before
 * settings_manager_init, so main wires this separately (the
 * log_manager_register_settings pattern). Reboot-to-apply.
 */
#include "settings_manager.h"

#include "filesystem.h" /* filesystem_register_cli (settings-gated) */

static const settings_field_t FSM_SETTINGS_FIELDS[] =
{
    SETTINGS_BOOL("cli", true),
};

static esp_err_t fsm_settings_apply(const cJSON *settings)
{
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && filesystem_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}

esp_err_t filesystem_register_settings(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "filesystem",
        .version     = 1,
        .fields      = FSM_SETTINGS_FIELDS,
        .field_count = sizeof(FSM_SETTINGS_FIELDS) /
                       sizeof(FSM_SETTINGS_FIELDS[0]),
        .on_apply    = fsm_settings_apply,
    };

    return settings_manager_register(&DESC);
}
