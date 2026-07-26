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
 * @file imu_manager_http.c
 * @brief The optional /api/imu route (§9.1: feature components register
 *        their own domain routes) — main calls imu_manager_register_http()
 *        only in HTTP compositions, so the chip code carries no HTTP
 *        dependency. Documented in components/HTTP_API.md §6e.
 */
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "imu_manager.h"

static const char *TAG = "imu_manager";

static esp_err_t imu_handler(httpd_req_t *req)
{
    imu_manager_activity_t act = imu_manager_activity();
    float ax, ay, az, temp;
    char accel[80] = "null";
    char temp_s[16] = "null";
    char body[160];

    if (imu_manager_read_accel(&ax, &ay, &az) == ESP_OK)
    {
        snprintf(accel, sizeof(accel),
                 "{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}", ax, ay, az);
    }

    if (imu_manager_read_temp(&temp) == ESP_OK)
    {
        snprintf(temp_s, sizeof(temp_s), "%.1f", temp);
    }

    snprintf(body, sizeof(body),
             "{\"activity\":\"%s\",\"accel\":%s,\"temp\":%s}",
             (act == IMU_MANAGER_ACTIVE) ? "active" :
             (act == IMU_MANAGER_STATIONARY) ? "stationary" : "unknown",
             accel, temp_s);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t imu_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/imu", .method = HTTP_GET, .handler = imu_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/imu registered");
    }

    return err;
}
