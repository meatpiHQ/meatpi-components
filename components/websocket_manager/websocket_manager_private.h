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
 * @file websocket_manager_private.h
 * @brief Internal API + the pure policy layer (host-testable — no httpd).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure policy (websocket_manager_policy.c — host-tested) ---------------- */

typedef struct
{
    bool     enabled;
    bool     text_mode;      /* frame type: false = binary (default)     */
    char     name[16];
    char     path[32];       /* must start with "/ws/"                   */
    uint8_t  max_clients;
} wsm_channel_cfg_t;

/** Parse one settings array item (code defaults: max_clients 2, binary,
 *  disabled). Pure. */
esp_err_t wsm_parse_channel(const cJSON *item, wsm_channel_cfg_t *out);

/** Cross-item rules: unique names, unique paths, "/ws/" prefix (the /api
 *  namespace stays api_http's; assets own the rest), max_clients bounds. */
esp_err_t wsm_validate_channels(const cJSON *channels, char *err,
                                size_t err_len);

/** v1 -> v2: append the ws_log default channel to a stored channel array
 *  (skipped when the name/path is taken or the table is full). Pure. */
esp_err_t wsm_migrate_channels(uint32_t from_version, cJSON *settings);

/* ---- settings (websocket_manager_settings.c) -------------------------------- */

/** Register the "websocket_manager" descriptor with settings_manager. */
esp_err_t wsm_settings_register(void);

const wsm_channel_cfg_t *wsm_core_config(int idx); /* NULL past end */

int  wsm_settings_count(void);         /* configured channel slots          */
bool wsm_settings_is_configured(void); /* boot apply ran (standard §4.3)    */

/* ---- ws layer (websocket_manager_ws.c) — indexed like the config ----------- */

esp_err_t wsm_ws_register_routes(void);
void      wsm_ws_reset(void);
esp_err_t wsm_ws_subscribe(int idx, void *queue);
esp_err_t wsm_ws_unsubscribe(int idx, void *queue);
esp_err_t wsm_ws_send(int idx, const uint8_t *data, size_t len);
esp_err_t wsm_ws_stats(int idx, void *stats_out);

#ifdef __cplusplus
}
#endif
