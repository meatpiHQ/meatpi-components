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
 * @file usb_host_manager_http.c
 * @brief The optional /api/usb status route (§9.1).
 */
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "usb_host_manager.h"

static const char *TAG = "usb_host_manager";

static esp_err_t usb_handler(httpd_req_t *req)
{
    usb_host_manager_status_t st;
    char body[288];

    (void)usb_host_manager_status(&st);
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"device_present\":%s,\"host_active\":%s,"
             "\"eth_connected\":%s,\"driver\":\"%s\",\"ip\":\"%s\","
             "\"attaches\":%lu,\"vid\":\"%04x\",\"pid\":\"%04x\","
             "\"vbus\":%s}",
             st.enabled ? "true" : "false",
             st.device_present ? "true" : "false",
             st.host_active ? "true" : "false",
             st.eth_connected ? "true" : "false",
             st.driver, st.ip, (unsigned long)st.attaches,
             st.vid, st.pid, st.vbus_on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t usb_host_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/usb", .method = HTTP_GET,
          .handler = usb_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/usb registered");
    }

    return err;
}
