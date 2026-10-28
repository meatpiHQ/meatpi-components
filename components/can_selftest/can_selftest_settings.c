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
 * @file can_selftest_settings.c
 * @brief settings_manager descriptor for can_selftest (CLI ownership, §6b):
 *        the one knob is whether the `can_selftest` console command exists.
 *        Reboot-to-apply, like every setting.
 */
#include "settings_manager.h"

#include "can_selftest.h"

static const settings_field_t CST_FIELDS[] =
{
    SETTINGS_BOOL("cli", true),
};

static esp_err_t cst_on_apply(const cJSON *settings)
{
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && can_selftest_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    return ESP_OK;
}

esp_err_t can_selftest_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "can_selftest",
        .version     = 1,
        .fields      = CST_FIELDS,
        .field_count = sizeof(CST_FIELDS) / sizeof(CST_FIELDS[0]),
        .on_apply    = cst_on_apply,
    };

    return settings_manager_register(&DESC);
}
