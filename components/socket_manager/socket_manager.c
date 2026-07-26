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
 * @file socket_manager.c
 * @brief Lifecycle and the name-based public API (subscribe/send/stats route
 *        to the net layer by slot index). Settings live in
 *        socket_manager_settings.c (standard §4.1).
 */
#include "socket_manager.h"

#include <string.h>

#include "esp_log.h"

#include "log_manager.h"

#include "socket_manager_private.h"

static const char *TAG = "socket_manager";

static bool s_started;

static int find_server(const char *name)
{
    for (int i = 0; name != NULL; i++)
    {
        const smp_server_cfg_t *cfg = sm_core_config(i);

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

esp_err_t socket_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "socket_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return sm_settings_register();
}

esp_err_t socket_manager_start(void)
{
    if (!sm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    esp_err_t err = sm_net_start();

    if (err == ESP_OK)
    {
        s_started = true;
        ESP_LOGI(TAG, "started (%d server slots configured)",
                 sm_settings_count());
    }

    return err;
}

esp_err_t socket_manager_stop(void)
{
    if (!s_started)
    {
        return ESP_OK;
    }

    sm_net_stop();
    s_started = false;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

/* ---- name-based API -------------------------------------------------------------- */

esp_err_t socket_manager_subscribe(const char *server, QueueHandle_t q)
{
    int idx = find_server(server);

    return (idx < 0) ? ESP_ERR_NOT_FOUND : sm_net_subscribe(idx, q);
}

esp_err_t socket_manager_unsubscribe(const char *server, QueueHandle_t q)
{
    int idx = find_server(server);

    return (idx < 0) ? ESP_ERR_NOT_FOUND : sm_net_unsubscribe(idx, q);
}

esp_err_t socket_manager_send(const char *server, const uint8_t *data,
                              size_t len)
{
    int idx = find_server(server);

    if (idx < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return sm_net_send(idx, data, len);
}

esp_err_t socket_manager_stats(const char *server, socket_stats_t *out)
{
    int idx = find_server(server);

    return (idx < 0) ? ESP_ERR_NOT_FOUND : sm_net_stats(idx, out);
}

const char *socket_manager_server_name(int idx)
{
    const smp_server_cfg_t *cfg = sm_core_config(idx);

    return (cfg != NULL) ? cfg->name : NULL;
}
