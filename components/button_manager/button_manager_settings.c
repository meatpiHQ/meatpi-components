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
 * @file button_manager_settings.c
 * @brief settings_manager descriptor for button_manager: field-table
 *        schema (source of truth) and on_apply into the boot-applied
 *        config. No CLI commands — no `cli` key.
 */
#include "settings_manager.h"

#include "button_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", true),
    /* seconds of continuous hold (1 s poll ticks) that enter config
       mode — the legacy CONFIG_MODE_HOLD_SECONDS */
    SETTINGS_INT("hold_s", 1, 30, 5),
};

static bool     s_enabled;
static uint32_t s_hold_s;
static bool     s_configured;

bool btn_settings_enabled(void)
{
    return s_enabled;
}

uint32_t btn_settings_hold_s(void)
{
    return s_hold_s;
}

bool btn_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item;

    s_enabled = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "hold_s");
    s_hold_s = (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 5);

    s_configured = true;
    return ESP_OK;
}

esp_err_t btn_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "button_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
