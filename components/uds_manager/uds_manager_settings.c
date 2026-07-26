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
 * @file uds_manager_settings.c
 * @brief settings_manager descriptor for uds_manager: field-table schema
 *        (source of truth for shape/ranges/defaults) and on_apply (fills
 *        the boot-applied backend/timing config).
 */
#include <string.h>

#include "settings_manager.h"

#include "uds_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_STR_ENUM("backend", "auto,obd_chip,elm327,isotp", "auto"),
    SETTINGS_INT("p2_ms", 50, 5000, 250),
    SETTINGS_INT("p2star_ms", 500, 30000, 5000),
    SETTINGS_INT("tester_present_ms", 500, 10000, 2000),
    SETTINGS_BOOL("cli", true),
};

static uds_config_t s_cfg =
{
    .backend           = UDS_BACKEND_AUTO,
    .p2_ms             = 250,
    .p2star_ms         = 5000,
    .tester_present_ms = 2000,
};
static bool s_configured;

const uds_config_t *uds_settings_config(void)
{
    return &s_cfg;
}

bool uds_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, "backend");
    const char *b = (cJSON_IsString(v) && v->valuestring) ? v->valuestring
                                                          : "auto";

    if (strcmp(b, "obd_chip") == 0)    s_cfg.backend = UDS_BACKEND_OBD_CHIP;
    else if (strcmp(b, "elm327") == 0) s_cfg.backend = UDS_BACKEND_ELM327;
    else if (strcmp(b, "isotp") == 0)  s_cfg.backend = UDS_BACKEND_ISOTP;
    else                               s_cfg.backend = UDS_BACKEND_AUTO;

    v = cJSON_GetObjectItemCaseSensitive(settings, "p2_ms");
    s_cfg.p2_ms = cJSON_IsNumber(v) ? (uint32_t)v->valueint : 250;
    v = cJSON_GetObjectItemCaseSensitive(settings, "p2star_ms");
    s_cfg.p2star_ms = cJSON_IsNumber(v) ? (uint32_t)v->valueint : 5000;
    v = cJSON_GetObjectItemCaseSensitive(settings, "tester_present_ms");
    s_cfg.tester_present_ms = cJSON_IsNumber(v) ? (uint32_t)v->valueint
                                                : 2000;

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && uds_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t uds_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "uds_manager",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
