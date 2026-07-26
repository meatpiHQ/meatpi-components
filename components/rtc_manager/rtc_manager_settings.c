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
 * @file rtc_manager_settings.c
 * @brief settings_manager descriptor for rtc_manager: field-table schema
 *        (source of truth for shape/ranges/defaults) and on_apply (parses
 *        into the boot-applied config read through the rtcm_settings_*
 *        getters).
 */
#include <stdio.h>

#include "settings_manager.h"

#include "rtc_manager.h" /* rtc_manager_register_cli (settings-gated) */

#include "rtc_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", true),
    SETTINGS_BOOL("sntp", true),
    SETTINGS_STR("ntp_server", 63, "pool.ntp.org"),
    SETTINGS_STR("ntp_server2", 63, ""), /* fallback; "" = none         */
    SETTINGS_INT("sync_interval_h", 1, 168, 24),
    SETTINGS_BOOL("cli", true),
};

static rtcm_config_t s_config; /* boot-applied settings */
static bool s_configured;

const rtcm_config_t *rtcm_settings_config(void)
{
    return &s_config;
}

bool rtcm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    rtcm_config_t *c = &s_config;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings,
                                                         "enabled");

    c->enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(settings, "sntp");
    c->sntp = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(settings, "ntp_server");
    snprintf(c->ntp_server, sizeof(c->ntp_server), "%s",
             cJSON_IsString(item) ? item->valuestring : "pool.ntp.org");
    item = cJSON_GetObjectItemCaseSensitive(settings, "ntp_server2");
    snprintf(c->ntp_server2, sizeof(c->ntp_server2), "%s",
             cJSON_IsString(item) ? item->valuestring : "");
    item = cJSON_GetObjectItemCaseSensitive(settings, "sync_interval_h");
    c->sync_interval_h =
        (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 24);

    /* CLI ownership: settings-gated self-registration (reboot-to-apply) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && rtc_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t rtcm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "rtc_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
