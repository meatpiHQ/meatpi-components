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
 * @file can_manager_http.c
 * @brief GET /api/can — bus status + stats (§9.1 own-routes).
 */
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "can_manager.h"

static const char *TAG = "can_manager";

static const char *bus_state_str(uint8_t state)
{
    switch (state)
    {
        case CAN_CORE_BUS_RUNNING:    return "running";
        case CAN_CORE_BUS_OFF:        return "bus_off";
        case CAN_CORE_BUS_RECOVERING: return "recovering";
        default:                        return "stopped";
    }
}

static esp_err_t can_get_handler(httpd_req_t *req)
{
    can_manager_status_t st;
    char body[448];

    (void)can_manager_status(&st);
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"running\":%s,\"silent\":%s,"
             "\"baud_kbps\":%lu,\"state\":\"%s\","
             "\"tx\":%lu,\"rx\":%lu,\"tx_errors\":%lu,\"rx_errors\":%lu,"
             "\"arb_lost\":%lu,\"bus_errors\":%lu,"
             "\"rx_missed\":%lu,\"dispatch_drops\":%lu,"
             "\"bus_off\":%lu,\"recoveries\":%lu}",
             st.enabled ? "true" : "false",
             st.running ? "true" : "false",
             st.silent ? "true" : "false",
             (unsigned long)st.baud_kbps,
             st.running ? bus_state_str(st.stats.bus_state) : "stopped",
             (unsigned long)st.stats.tx_count,
             (unsigned long)st.stats.rx_count,
             (unsigned long)st.stats.tx_errors,
             (unsigned long)st.stats.rx_errors,
             (unsigned long)st.stats.arb_lost,
             (unsigned long)st.stats.bus_errors,
             (unsigned long)st.stats.rx_missed,
             (unsigned long)st.stats.dispatch_drops,
             (unsigned long)st.stats.bus_off_count,
             (unsigned long)st.stats.recovery_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t can_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/can", .method = HTTP_GET,
          .handler = can_get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/can registered");
    }

    return err;
}
