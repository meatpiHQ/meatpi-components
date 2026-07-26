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
 * @file websocket_manager.c
 * @brief Lifecycle and the name-based public API (routes to the ws layer by
 *        channel index). Settings live in websocket_manager_settings.c
 *        (standard §4.1).
 */
#include "websocket_manager.h"

#include <string.h>

#include "esp_log.h"

#include "log_manager.h"

#include "websocket_manager_private.h"

static const char *TAG = "websocket_manager";

static bool s_started;

static int find_channel(const char *name)
{
    for (int i = 0; name != NULL; i++)
    {
        const wsm_channel_cfg_t *cfg = wsm_core_config(i);

        if (cfg == NULL)
        {
            break;
        }

        if (strcmp(cfg->name, name) == 0)
        {
            return i;
        }
    }

    return -1;
}

/* ---- lifecycle ----------------------------------------------------------------- */

esp_err_t websocket_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "websocket_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return wsm_settings_register();
}

esp_err_t websocket_manager_start(void)
{
    if (!wsm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    wsm_ws_reset();

    esp_err_t err = wsm_ws_register_routes();

    if (err == ESP_OK)
    {
        s_started = true;
        ESP_LOGI(TAG, "started (%d channel slots configured)",
                 wsm_settings_count());
    }

    return err;
}

esp_err_t websocket_manager_stop(void)
{
    /* routes live as long as the server; drop the client tables */
    wsm_ws_reset();
    s_started = false;
    return ESP_OK;
}

/* ---- name-based API -------------------------------------------------------------- */

esp_err_t websocket_manager_subscribe(const char *channel, QueueHandle_t q)
{
    int idx = find_channel(channel);

    return (idx < 0) ? ESP_ERR_NOT_FOUND : wsm_ws_subscribe(idx, q);
}

esp_err_t websocket_manager_unsubscribe(const char *channel, QueueHandle_t q)
{
    int idx = find_channel(channel);

    return (idx < 0) ? ESP_ERR_NOT_FOUND : wsm_ws_unsubscribe(idx, q);
}

esp_err_t websocket_manager_send(const char *channel, const uint8_t *data,
                                 size_t len)
{
    int idx = find_channel(channel);

    if (idx < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return wsm_ws_send(idx, data, len);
}

esp_err_t websocket_manager_stats(const char *channel,
                                  websocket_stats_t *out)
{
    int idx = find_channel(channel);

    return (idx < 0) ? ESP_ERR_NOT_FOUND : wsm_ws_stats(idx, out);
}

const char *websocket_manager_channel_name(int idx)
{
    const wsm_channel_cfg_t *cfg = wsm_core_config(idx);

    return (cfg != NULL) ? cfg->name : NULL;
}
