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
 * @file obd_gate_settings.c
 * @brief settings_manager descriptor for obd_gate: field-table schema
 *        (source of truth for shape/defaults) and on_apply (fills the
 *        boot-applied enable flag).
 *
 * `enabled` default TRUE (meatpi 2026-07-11): the MIC chip and the
 * ESP-side ELM engines share one physical CAN bus, and overlapping
 * request/response conversations mis-attribute responses (a BLE app
 * polling the chip + autopid on the elm327 backend = bad data). Disable
 * only for benches that WANT concurrent conversations.
 */
#include "settings_manager.h"

#include "obd_gate_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", true),
};

/* boot-applied settings */
static bool s_enabled = true; /* permissive pre-apply default matches schema */
static bool s_configured;

bool og_settings_enabled(void)
{
    return s_enabled;
}

bool og_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_enabled = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    s_configured = true;
    return ESP_OK;
}

esp_err_t og_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "obd_gate",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
