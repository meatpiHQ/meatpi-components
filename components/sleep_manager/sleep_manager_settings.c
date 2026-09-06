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
 * @file sleep_manager_settings.c
 * @brief settings_manager descriptor for sleep_manager: field-table
 *        schema (source of truth for shape/ranges/defaults) and
 *        on_apply (parses into the boot-applied policy config).
 *
 * Symbol note: the getters use a `sleep_` prefix, not the component's
 * usual `sm_` — socket_manager already links sm_settings_*.
 */
#include "settings_manager.h"

#include "sleep_manager.h" /* sleep_manager_register_cli (settings-gated) */
#include "sleep_manager_private.h"

static const settings_field_t FIELDS[] =
{
    /* shipping default ON, 5 min delay (meatpi 2026-07-18): a parked
       device must not drain the car battery out of the box */
    SETTINGS_BOOL("enabled", true),
    /* user-facing ranges (meatpi 2026-09-06): a 12 V vehicle battery —
       below 12 V it is flat (meatpi: 12.0 V floor), above ~14 V the engine
       is charging; a
       sleep delay beyond 30 min only drains the battery; a periodic
       check-in faster than 5 min is a wake-up storm */
    SETTINGS_INT("sleep_mv", 12000, 14000, 13100),
    SETTINGS_INT("sleep_delay_min", 1, 30, 5),
    SETTINGS_BOOL("periodic_wakeup", false),
    SETTINGS_INT("wakeup_interval_min", 5, 1440, 30),
    SETTINGS_BOOL("cli", true),
};

static sm_cfg_t s_cfg;
static bool s_enabled;
static bool s_configured;

const sm_cfg_t *sleep_settings_config(void)
{
    return &s_cfg;
}

bool sleep_settings_enabled(void)
{
    return s_enabled;
}

bool sleep_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item;

    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "sleep_mv");
    s_cfg.sleep_v = (cJSON_IsNumber(item) ? item->valueint : 13100)
                    / 1000.0f;
    s_cfg.wake_v = s_cfg.sleep_v + SM_WAKE_DELTA_V;
    item = cJSON_GetObjectItemCaseSensitive(settings, "sleep_delay_min");
    s_cfg.delay_ms = 60000u *
        (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 5);
    s_cfg.periodic = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "periodic_wakeup"));
    item = cJSON_GetObjectItemCaseSensitive(settings,
                                            "wakeup_interval_min");
    s_cfg.interval_ms = 60000u *
        (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 30);

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && sleep_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}


/* Clamp a stored integer into the current schema range (migration helper). */
static void clamp_int(cJSON *settings, const char *key, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    if (cJSON_IsNumber(v) && (v->valueint < lo || v->valueint > hi))
    {
        cJSON_SetNumberValue(v, (v->valueint < lo) ? lo : hi);
    }
}

static esp_err_t sleep_settings_migrate(uint32_t from_version, cJSON *settings)
{
    /* v1 -> v2 (2026-09-06): tighter user-facing ranges — clamp what a
       device has stored instead of degrading it */
    if (from_version < 2 && settings != NULL)
    {
        clamp_int(settings, "sleep_mv", 12000, 14000);
        clamp_int(settings, "sleep_delay_min", 1, 30);
        clamp_int(settings, "wakeup_interval_min", 5, 1440);
    }

    return ESP_OK;
}

esp_err_t sleep_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "sleep_manager",
        .version = 2, /* v2: sane ranges (2026-09-06) */
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_migrate = sleep_settings_migrate,
    };

    return settings_manager_register(&DESC);
}
