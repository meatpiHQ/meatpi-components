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
 * @file mdns_manager_settings.c
 * @brief settings_manager descriptor for mdns_manager: field-table schema
 *        (source of truth for shape/defaults) and on_apply (stores the
 *        boot-applied enabled flag read by _start()).
 */
#include "settings_manager.h"

#include "mdns_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", true),
};

/* boot-applied settings */
static bool s_enabled;
static bool s_configured;

bool mdns_manager_settings_enabled(void)
{
    return s_enabled;
}

bool mdns_manager_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(settings, "enabled");

    s_enabled = cJSON_IsTrue(item);
    s_configured = true;
    return ESP_OK;
}

esp_err_t mdns_manager_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "mdns_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
