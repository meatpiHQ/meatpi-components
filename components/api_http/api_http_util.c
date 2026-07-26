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
 * @file api_http_util.c
 * @brief Pure helpers for the HTTP API glue — no esp_http_server dependency,
 *        so the whole file host-tests on the IDF linux target.
 */
#include <string.h>

#include "esp_log.h"

#include "api_http_private.h"

#define API_SETTINGS_PREFIX "/api/settings/"

static bool key_is_password(const char *key)
{
    /* secret-field suffixes: redacted in settings GETs, "" on PUT means
     * keep-stored. `public_key` fields deliberately NOT here — peers'
     * public keys are display data (vpn_manager) */
    static const char *const SUFFIXES[] =
        { "_password", "private_key", "preshared_key", "auth_key" };
    size_t klen = strlen(key);

    for (size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++)
    {
        size_t slen = strlen(SUFFIXES[i]);

        if (klen >= slen && strcmp(key + klen - slen, SUFFIXES[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

void api_util_redact(cJSON *obj)
{
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, obj)
    {
        if (item->string != NULL && cJSON_IsString(item) &&
            key_is_password(item->string))
        {
            cJSON_SetValuestring(item, "");
        }
    }
}

void api_util_unredact(cJSON *in, const cJSON *stored)
{
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, in)
    {
        if (item->string == NULL || !cJSON_IsString(item) ||
            !key_is_password(item->string) ||
            item->valuestring == NULL || item->valuestring[0] != '\0')
        {
            continue;
        }

        const cJSON *kept = cJSON_GetObjectItemCaseSensitive(stored,
                                                             item->string);

        if (cJSON_IsString(kept) && kept->valuestring != NULL)
        {
            cJSON_SetValuestring(item, kept->valuestring);
        }
    }
}

esp_err_t api_util_settings_path(const char *uri, char *name,
                                 size_t name_len, bool *is_schema)
{
    static const char SCHEMA[] = "/schema";
    size_t prefix_len = sizeof(API_SETTINGS_PREFIX) - 1;

    if (uri == NULL || name == NULL || is_schema == NULL ||
        strncmp(uri, API_SETTINGS_PREFIX, prefix_len) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *rest = uri + prefix_len;
    size_t rest_len = strlen(rest);

    *is_schema = false;

    if (rest_len > sizeof(SCHEMA) - 1 &&
        strcmp(rest + rest_len - (sizeof(SCHEMA) - 1), SCHEMA) == 0)
    {
        *is_schema = true;
        rest_len -= sizeof(SCHEMA) - 1;
    }

    if (rest_len == 0 || rest_len >= name_len ||
        memchr(rest, '/', rest_len) != NULL)
    {
        return ESP_ERR_INVALID_ARG; /* empty, oversized, or extra segments */
    }

    memcpy(name, rest, rest_len);
    name[rest_len] = '\0';
    return ESP_OK;
}

esp_err_t api_util_level_from_str(const char *s, int *out_level)
{
    static const struct
    {
        const char *name;
        int         level;
    } MAP[] =
    {
        { "none",    ESP_LOG_NONE    },
        { "error",   ESP_LOG_ERROR   },
        { "warn",    ESP_LOG_WARN    },
        { "info",    ESP_LOG_INFO    },
        { "debug",   ESP_LOG_DEBUG   },
        { "verbose", ESP_LOG_VERBOSE },
    };

    if (s == NULL || out_level == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
    {
        if (strcmp(s, MAP[i].name) == 0)
        {
            *out_level = MAP[i].level;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
