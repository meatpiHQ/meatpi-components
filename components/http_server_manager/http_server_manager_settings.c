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
 * @file http_server_manager_settings.c
 * @brief settings_manager descriptor for http_server_manager: the admin
 *        password (meatpi 2026-07-19 — the basic-password successor to
 *        the parked pairing-token design). Disabled by default; when
 *        enabled every inbound HTTP request / WS handshake must present
 *        the password (Basic/Bearer header or `wican_auth` cookie).
 *        `auth_password` ends in `_password` -> the api_http redaction
 *        rules cover it automatically (GET returns "", "" on PUT keeps
 *        the stored secret). Reboot-to-apply like all settings.
 */
#include <string.h>

#include "settings_manager.h"

#include "http_server_manager_private.h"

static const settings_field_t FIELDS[] =
{
    /* the gate switch: off = open device (the historic behavior) */
    SETTINGS_BOOL("auth_enabled", false),
    SETTINGS_STR ("auth_password", 64, ""),
};

static bool s_enabled;
static char s_password[65];

bool hsm_auth_enabled(void)
{
    return s_enabled && s_password[0] != '\0';
}

const char *hsm_auth_password(void)
{
    return s_password;
}

static esp_err_t on_validate(const cJSON *settings, char *err,
                             size_t err_len)
{
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(settings,
                                                       "auth_enabled");
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(settings,
                                                       "auth_password");

    if (cJSON_IsTrue(en) &&
        (!cJSON_IsString(pw) || strlen(pw->valuestring) < 4))
    {
        snprintf(err, err_len,
                 "auth_enabled requires auth_password (>= 4 chars)");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item;

    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "auth_enabled"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "auth_password");
    s_password[0] = '\0';

    if (cJSON_IsString(item))
    {
        strncpy(s_password, item->valuestring, sizeof(s_password) - 1);
        s_password[sizeof(s_password) - 1] = '\0';
    }

    return ESP_OK;
}

esp_err_t hsm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "http_server_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}
