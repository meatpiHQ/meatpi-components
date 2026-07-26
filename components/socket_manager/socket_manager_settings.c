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
 * @file socket_manager_settings.c
 * @brief settings_manager descriptor for socket_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_validate (cross-item
 *        rules), on_apply (parses into the boot-applied server table).
 */
#include "esp_attr.h"

#include "settings_manager.h"

#include "socket_manager.h"
#include "socket_manager_private.h"

/* clang-format off */
static const settings_field_t SERVER_ITEMS[] =
{
    SETTINGS_STR_REQ     ("name",  1, 15, ""),
    SETTINGS_STR_ENUM_REQ("proto", "tcp,udp", "tcp"),
    SETTINGS_INT_REQ     ("port",  1, 65535, 3333),
    /* optional-key defaults mirror smp_parse_server's code defaults */
    SETTINGS_INT         ("max_clients", 1, 4, 2),
    SETTINGS_INT         ("keepalive_s", 0, 600, 30),
    SETTINGS_BOOL        ("enabled", false),
};

/* Defaults per meatpi 2026-07-03: obd0 TCP:35000 enabled; slcan0 TCP:3333,
   gvret0 TCP:23, udp0 UDP:17 parked (disabled). */
static const settings_field_t FIELDS[] =
{
    SETTINGS_ARRAY("servers", SOCKET_MANAGER_MAX_SERVERS, SERVER_ITEMS,
        SETTINGS_JSON([
            {"name":"obd0",  "proto":"tcp","port":35000,"enabled":true},
            {"name":"slcan0","proto":"tcp","port":3333, "enabled":false},
            {"name":"gvret0","proto":"tcp","port":23,   "enabled":false},
            {"name":"udp0",  "proto":"udp","port":17,   "enabled":false}
        ])),
};
/* clang-format on */

static smp_server_cfg_t s_cfg[SOCKET_MANAGER_MAX_SERVERS] EXT_RAM_BSS_ATTR;
static int  s_cfg_count;
static bool s_configured;

const smp_server_cfg_t *sm_core_config(int idx)
{
    return (idx >= 0 && idx < s_cfg_count) ? &s_cfg[idx] : NULL;
}

int sm_settings_count(void)
{
    return s_cfg_count;
}

bool sm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_validate(const cJSON *settings, char *err, size_t err_len)
{
    const cJSON *servers = cJSON_GetObjectItemCaseSensitive(settings,
                                                            "servers");

    if (!cJSON_IsArray(servers))
    {
        return ESP_OK; /* schema already enforced the type when present */
    }

    return smp_validate_servers(servers, err, err_len);
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *servers = cJSON_GetObjectItemCaseSensitive(settings,
                                                            "servers");
    const cJSON *item = NULL;

    s_cfg_count = 0;

    cJSON_ArrayForEach(item, servers)
    {
        if (s_cfg_count >= SOCKET_MANAGER_MAX_SERVERS)
        {
            break; /* validated earlier; belt-and-braces */
        }

        if (smp_parse_server(item, &s_cfg[s_cfg_count]) == ESP_OK)
        {
            s_cfg_count++;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t sm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "socket_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}
