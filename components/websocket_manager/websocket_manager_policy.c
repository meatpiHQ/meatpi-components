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
 * @file websocket_manager_policy.c
 * @brief Pure policy: channel config parsing + cross-item validation.
 *        No httpd — host-tests on the linux target.
 */
#include <stdio.h>
#include <string.h>

#include "websocket_manager.h"
#include "websocket_manager_private.h"

static bool name_is_valid(const char *s, size_t max_len)
{
    size_t len = strlen(s);

    if (len == 0 || len >= max_len)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_';

        if (!ok)
        {
            return false;
        }
    }

    return true;
}

esp_err_t wsm_parse_channel(const cJSON *item, wsm_channel_cfg_t *out)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
    const cJSON *maxc = cJSON_GetObjectItemCaseSensitive(item, "max_clients");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(item, "mode");
    const cJSON *ena = cJSON_GetObjectItemCaseSensitive(item, "enabled");

    if (!cJSON_IsString(name) || !cJSON_IsString(path) ||
        !name_is_valid(name->valuestring, sizeof(out->name)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* the /ws/ namespace is this component's; /api stays api_http's and
       the asset catch-all owns everything else */
    if (strncmp(path->valuestring, "/ws/", 4) != 0 ||
        strlen(path->valuestring) >= sizeof(out->path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    strcpy(out->name, name->valuestring);
    strcpy(out->path, path->valuestring);
    out->max_clients = cJSON_IsNumber(maxc) ? (uint8_t)maxc->valuedouble : 2;
    out->text_mode = cJSON_IsString(mode) &&
                     strcmp(mode->valuestring, "text") == 0;
    out->enabled = cJSON_IsTrue(ena);
    return ESP_OK;
}

esp_err_t wsm_migrate_channels(uint32_t from_version, cJSON *settings)
{
    if (from_version >= 2)
    {
        return ESP_OK;
    }

    /* v1 -> v2: the ws_log channel (log_sinks' live log stream) joins
     * the defaults. Stored arrays don't schema-fill, so migration
     * appends it — unless the user already claimed the name or the
     * table is full. Ships ENABLED: the route is just registered; no
     * log line flows until log_sinks' ws gate (default false) opens. */
    cJSON *channels = cJSON_GetObjectItemCaseSensitive(settings,
                                                       "channels");

    if (!cJSON_IsArray(channels) ||
        cJSON_GetArraySize(channels) >= WEBSOCKET_MANAGER_MAX_CHANNELS)
    {
        return ESP_OK;
    }

    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, channels)
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");

        if ((cJSON_IsString(name) &&
             strcmp(name->valuestring, "ws_log") == 0) ||
            (cJSON_IsString(path) &&
             strcmp(path->valuestring, "/ws/log") == 0))
        {
            return ESP_OK; /* user already owns the name/path: keep it */
        }
    }

    cJSON *entry = cJSON_CreateObject();

    if (entry == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(entry, "name", "ws_log");
    cJSON_AddStringToObject(entry, "path", "/ws/log");
    cJSON_AddStringToObject(entry, "mode", "text");
    cJSON_AddBoolToObject(entry, "enabled", true);
    cJSON_AddItemToArray(channels, entry);
    return ESP_OK;
}

esp_err_t wsm_validate_channels(const cJSON *channels, char *err,
                                size_t err_len)
{
    wsm_channel_cfg_t cfg[WEBSOCKET_MANAGER_MAX_CHANNELS];
    int count = 0;
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, channels)
    {
        if (count >= WEBSOCKET_MANAGER_MAX_CHANNELS)
        {
            snprintf(err, err_len, "more than %d channels",
                     WEBSOCKET_MANAGER_MAX_CHANNELS);
            return ESP_ERR_INVALID_ARG;
        }

        if (wsm_parse_channel(item, &cfg[count]) != ESP_OK)
        {
            snprintf(err, err_len,
                     "channels[%d]: bad name or path (must be /ws/...)",
                     count);
            return ESP_ERR_INVALID_ARG;
        }

        if (cfg[count].max_clients < 1 ||
            cfg[count].max_clients > WEBSOCKET_MANAGER_MAX_CLIENTS)
        {
            snprintf(err, err_len, "channels[%d]: max_clients 1..%d", count,
                     WEBSOCKET_MANAGER_MAX_CLIENTS);
            return ESP_ERR_INVALID_ARG;
        }

        for (int i = 0; i < count; i++)
        {
            if (strcmp(cfg[i].name, cfg[count].name) == 0)
            {
                snprintf(err, err_len, "duplicate channel name '%s'",
                         cfg[count].name);
                return ESP_ERR_INVALID_ARG;
            }

            if (strcmp(cfg[i].path, cfg[count].path) == 0)
            {
                snprintf(err, err_len, "duplicate path '%s'",
                         cfg[count].path);
                return ESP_ERR_INVALID_ARG;
            }
        }

        count++;
    }

    return ESP_OK;
}
