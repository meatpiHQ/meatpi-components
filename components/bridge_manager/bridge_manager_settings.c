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
 * @file bridge_manager_settings.c
 * @brief settings_manager descriptor for bridge_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_validate (registry +
 *        cross-item rules), on_apply (parses into the boot-applied bridge
 *        table).
 */
#include "esp_attr.h"

#include "settings_manager.h"

#include "bridge_manager.h"
#include "bridge_manager_private.h"

/* clang-format off */
static const settings_field_t BRIDGE_ITEMS[] =
{
    SETTINGS_STR_REQ("name", 1, 15, ""),
    SETTINGS_STR_REQ("a",    1, 15, ""),
    SETTINGS_STR_REQ("b",    1, 15, ""),
    /* optional-key defaults mirror bm_parse_bridge's code defaults */
    SETTINGS_STR_LEN("translator", 1, 15, "raw"),
    SETTINGS_BOOL   ("enabled", false),
};

/* Defaults (meatpi 2026-07-18, legacy-parity): the OBD chip reachable out
   of the box over TCP 35000 (socket_manager's default-enabled obd0 —
   the classic ELM327-WiFi-adapter convention 192.168.0.10:35000), the
   USB port-B UART (plug into a PC = serial ELM327), and the built-in web
   UI's terminal (ws_obd). All three ride the obd jack's multi_consumer
   fan-out (every subscriber gets a full RX copy; TX is serialized). BLE
   passthrough etc. are enabled from settings. */
static const settings_field_t FIELDS[] =
{
    SETTINGS_ARRAY("bridges", BRIDGE_MANAGER_MAX_BRIDGES, BRIDGE_ITEMS,
        SETTINGS_JSON([
            {"name":"br_obd","a":"obd","b":"ws_obd",
             "translator":"raw","enabled":true},
            {"name":"br_tcp_obd","a":"obd0","b":"obd",
             "translator":"raw","enabled":true},
            {"name":"br_usb_obd","a":"usb_obd","b":"obd",
             "translator":"raw","enabled":true}
        ])),
};
/* clang-format on */

static bm_bridge_cfg_t s_cfg[BRIDGE_MANAGER_MAX_BRIDGES] EXT_RAM_BSS_ATTR;
static int  s_cfg_count;
static bool s_configured;

const bm_bridge_cfg_t *bm_core_bridge_cfg(int idx)
{
    return (idx >= 0 && idx < s_cfg_count) ? &s_cfg[idx] : NULL;
}

int bm_settings_count(void)
{
    return s_cfg_count;
}

bool bm_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_validate(const cJSON *settings, char *err, size_t err_len)
{
    const cJSON *bridges = cJSON_GetObjectItemCaseSensitive(settings,
                                                            "bridges");

    if (!cJSON_IsArray(bridges))
    {
        return ESP_OK;
    }

    /* BOOT apply (bridge_manager not started yet): the registry is not
       final — dynamic socket/WS jacks register at bridge_endpoints_start,
       between this pass and bridge_manager_start. Pass -1 = skip the
       existence checks (cross-item rules still apply); build_bridge
       degrades an unknown name alone. A runtime PUT is strict. */
    if (!bm_core_started())
    {
        return bm_validate_bridges(bridges, NULL, NULL, -1, NULL, -1,
                                   err, err_len);
    }

    const char *ep_names[BRIDGE_MANAGER_MAX_ENDPOINTS];
    bool ep_multi[BRIDGE_MANAGER_MAX_ENDPOINTS];
    const char *tr_names[BRIDGE_MANAGER_MAX_TRANSLATORS];
    int ep_count = bm_core_endpoint_names(ep_names, ep_multi,
                                          BRIDGE_MANAGER_MAX_ENDPOINTS);
    int tr_count = bm_core_translator_names(tr_names,
                                            BRIDGE_MANAGER_MAX_TRANSLATORS);

    return bm_validate_bridges(bridges, ep_names, ep_multi, ep_count,
                               tr_names, tr_count, err, err_len);
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *bridges = cJSON_GetObjectItemCaseSensitive(settings,
                                                            "bridges");
    const cJSON *item = NULL;

    s_cfg_count = 0;

    cJSON_ArrayForEach(item, bridges)
    {
        if (s_cfg_count >= BRIDGE_MANAGER_MAX_BRIDGES)
        {
            break;
        }

        if (bm_parse_bridge(item, &s_cfg[s_cfg_count]) == ESP_OK)
        {
            s_cfg_count++;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t bm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "bridge_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}
