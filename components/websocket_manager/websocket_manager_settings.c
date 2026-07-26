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
 * @file websocket_manager_settings.c
 * @brief settings_manager descriptor for websocket_manager: field-table
 *        schema (source of truth for shape/ranges/defaults), on_validate
 *        (cross-item rules), on_apply (parses into the boot-applied
 *        channel table).
 */
#include "esp_attr.h"

#include "settings_manager.h"

#include "websocket_manager.h"
#include "websocket_manager_private.h"

/* clang-format off */
static const settings_field_t CHANNEL_ITEMS[] =
{
    SETTINGS_STR_REQ("name", 1, 15, ""),
    SETTINGS_STR_REQ("path", 5, 31, "/ws/"),
    /* optional-key defaults mirror wsm_parse_channel's code defaults */
    SETTINGS_INT     ("max_clients", 1, 4, 2),
    SETTINGS_STR_ENUM("mode", "binary,text", "binary"),
    SETTINGS_BOOL    ("enabled", false),
};

/* Default channels for the known consumers. ws_obd ships ENABLED (paired
   with bridge_manager's default obd<->ws_obd bridge = OBD over WebSocket
   out of the box); ws_log ships enabled too — the ROUTE only: nothing
   flows until log_sinks' ws gate (default false) opens. The rest stay
   parked (disabled) until the UI/product enables them. */
static const settings_field_t FIELDS[] =
{
    SETTINGS_ARRAY("channels", WEBSOCKET_MANAGER_MAX_CHANNELS, CHANNEL_ITEMS,
        SETTINGS_JSON([
            {"name":"ws_obd","path":"/ws/obd","mode":"binary","enabled":true},
            {"name":"ws_can","path":"/ws/can","mode":"binary","enabled":false},
            {"name":"ws_cli","path":"/ws/cli","mode":"text",  "enabled":false},
            {"name":"ws_log","path":"/ws/log","mode":"text",  "enabled":true}
        ])),
};
/* clang-format on */

static wsm_channel_cfg_t s_cfg[WEBSOCKET_MANAGER_MAX_CHANNELS]
    EXT_RAM_BSS_ATTR;
static int  s_cfg_count;
static bool s_configured;

const wsm_channel_cfg_t *wsm_core_config(int idx)
{
    return (idx >= 0 && idx < s_cfg_count) ? &s_cfg[idx] : NULL;
}

int wsm_settings_count(void)
{
    return s_cfg_count;
}

bool wsm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_validate(const cJSON *settings, char *err, size_t err_len)
{
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(settings,
                                                             "channels");

    if (!cJSON_IsArray(channels))
    {
        return ESP_OK;
    }

    return wsm_validate_channels(channels, err, err_len);
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(settings,
                                                             "channels");
    const cJSON *item = NULL;

    s_cfg_count = 0;

    cJSON_ArrayForEach(item, channels)
    {
        if (s_cfg_count >= WEBSOCKET_MANAGER_MAX_CHANNELS)
        {
            break;
        }

        if (wsm_parse_channel(item, &s_cfg[s_cfg_count]) == ESP_OK)
        {
            s_cfg_count++;
        }
    }

    s_configured = true;
    return ESP_OK;
}

static esp_err_t on_migrate(uint32_t from_version, cJSON *settings)
{
    return wsm_migrate_channels(from_version, settings);
}

esp_err_t wsm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "websocket_manager",
        .version = 2, /* v2 2026-07-26: +ws_log default channel */
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_validate = on_validate,
        .on_migrate = on_migrate,
    };

    return settings_manager_register(&DESC);
}
