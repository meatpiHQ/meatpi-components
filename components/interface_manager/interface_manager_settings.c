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
 * @file interface_manager_settings.c
 * @brief settings_manager descriptor for interface_manager: field-table
 *        schema (source of truth for shape/defaults) and on_apply
 *        (fills the boot-applied rule toggles).
 */
#include "settings_manager.h"

#include "interface_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", true),
    SETTINGS_BOOL("sta_ble_handover", true),
    SETTINGS_BOOL("ap_ble_exclusive", true),
};

static im_config_t s_config;
static bool s_configured;

const im_config_t *im_settings_config(void)
{
    return &s_config;
}

bool im_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_config.enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    s_config.sta_ble_handover = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "sta_ble_handover"));
    s_config.ap_ble_exclusive = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "ap_ble_exclusive"));
    s_configured = true;
    return ESP_OK;
}

esp_err_t im_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "interface_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
