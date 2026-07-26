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
 * @file sleep_manager_http.c
 * @brief The optional /api/sleep status route (§9.1) — the UI's
 *        "sleep armed / countdown" banner reads this.
 */
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "sleep_manager.h"

static const char *TAG = "sleep_manager";

static const char *state_str(sleep_manager_state_t st)
{
    switch (st)
    {
        case SLEEP_MANAGER_LOW_VOLTAGE:  return "low_voltage";
        case SLEEP_MANAGER_SLEEPING:     return "sleeping";
        case SLEEP_MANAGER_WAKE_PENDING: return "wake_pending";
        default:                         return "normal";
    }
}

static esp_err_t sleep_handler(httpd_req_t *req)
{
    sleep_manager_status_t st;
    char body[192];

    (void)sleep_manager_status(&st);
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"state\":\"%s\",\"voltage\":%.2f,"
             "\"sleep_v\":%.2f,\"wake_v\":%.2f,\"naps\":%lu}",
             st.enabled ? "true" : "false", state_str(st.state),
             st.voltage, st.sleep_v, st.wake_v,
             (unsigned long)st.naps);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sleep_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/sleep", .method = HTTP_GET,
          .handler = sleep_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/sleep registered");
    }

    return err;
}
