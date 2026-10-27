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
 * @file obd_chip_http.c
 * @brief GET /api/obd_chip - the chip's wire and fan-out counters (own-routes
 *        pattern, §9.1; main calls obd_chip_register_http() in HTTP
 *        compositions). Read it before and after a stream to place a loss:
 *        chip -> UART ring (rx_overflows) -> fan-out (per-subscriber
 *        dropped) -> bridge (/api/bridges) -> socket (/api/sockets).
 *        Hand-formatted JSON: the document is small and fixed-shape, no
 *        cJSON dependency needed for it. Since 2026-10-28 (issue #1023) it
 *        also carries the chip firmware update's state (`fw`), and
 *        POST /api/obd_chip/update starts one (the packaged image or a
 *        file, `force` to reflash a chip at a current version): the one
 *        way to recover a chip stuck in its bootloader without a console.
 *        The GET never touches the wire.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "filesystem.h"
#include "http_server_manager.h"

#include "obd_chip.h"
#include "obd_chip_private.h"

static const char *TAG = "obd_chip";

#define OC_HTTP_SUBS 8
#define OC_HTTP_BODY_MAX 256

static esp_err_t obd_chip_get_handler(httpd_req_t *req)
{
    obd_chip_stats_t st;
    obd_chip_fw_status_t fw;
    obd_chip_sub_stats_t subs[OC_HTTP_SUBS];
    char body[1280];
    size_t n = 0;

    obd_chip_get_stats(&st);
    obd_chip_fw_status(&fw);

    size_t n_subs = obd_chip_get_subscribers(subs, OC_HTTP_SUBS);

    n += (size_t)snprintf(body + n, sizeof(body) - n,
                          "{\"ready\":%s,\"claim\":\"%s\","
                          "\"client_idle_ms\":%lu,\"baud\":%d,"
                          "\"eeprom_guard\":{\"rewrites\":%lu,\"blocked\":%lu,"
                          "\"protocol_saves\":%lu},"
                          "\"uart\":{\"rx_bytes\":%lu,\"rx_chunks\":%lu,"
                          "\"rx_max_chunk\":%u,\"rx_overflows\":%lu,"
                          "\"rx_buffered\":%lu,\"tx_bytes\":%lu},"
                          "\"fw\":{\"builtin\":\"%s\",\"current\":\"%s\","
                          "\"update\":{\"running\":%s,\"lines\":%lu,"
                          "\"total\":%lu,\"source\":\"%s\",\"done\":%s,"
                          "\"last\":\"%s\"}},"
                          "\"subscribers\":[",
                          st.ready ? "true" : "false", st.claim,
                          (unsigned long)st.client_idle_ms,
                          obd_chip_active_baud(),
                          (unsigned long)st.guard_rewrites,
                          (unsigned long)st.guard_blocked,
                          (unsigned long)st.protocol_saves,
                          (unsigned long)st.rx_bytes,
                          (unsigned long)st.rx_chunks,
                          (unsigned)st.rx_max_chunk,
                          (unsigned long)st.rx_overflows,
                          (unsigned long)st.rx_buffered,
                          (unsigned long)st.tx_bytes,
                          obd_chip_builtin_fw_version(),
                          obd_fw_current_versions(),
                          fw.running ? "true" : "false",
                          (unsigned long)fw.lines, (unsigned long)fw.total,
                          fw.source, fw.done_once ? "true" : "false",
                          fw.done_once ? esp_err_to_name(fw.last_err) : "");

    for (size_t i = 0; i < n_subs && n < sizeof(body) - 96; i++)
    {
        n += (size_t)snprintf(body + n, sizeof(body) - n,
                              "%s{\"idx\":%u,\"name\":\"%s\",\"dropped\":%lu,"
                              "\"queued\":%lu,\"depth\":%lu}",
                              (i > 0) ? "," : "", (unsigned)i, subs[i].name,
                              (unsigned long)subs[i].dropped,
                              (unsigned long)subs[i].queued,
                              (unsigned long)subs[i].depth);
    }

    n += (size_t)snprintf(body + n, sizeof(body) - n, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (ssize_t)n);
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/** POST /api/obd_chip/update  {"force":bool, "path":"/data/..."} (both
 *  optional; no body = the packaged image, not forced). 202 when the job
 *  started, 409 while one runs, 404 for a missing file, 400 otherwise. */
static esp_err_t obd_chip_update_handler(httpd_req_t *req)
{
    char body[OC_HTTP_BODY_MAX + 1];
    int len = 0;

    if (req->content_len > OC_HTTP_BODY_MAX)
    {
        return reply(req, "400 Bad Request", "{\"error\":\"body too large\"}");
    }

    while (len < (int)req->content_len)
    {
        int got = httpd_req_recv(req, body + len, req->content_len - (size_t)len);

        if (got <= 0)
        {
            return reply(req, "400 Bad Request", "{\"error\":\"body read\"}");
        }

        len += got;
    }

    body[len] = '\0';

    bool force = false;
    char path[72] = "";

    if (len > 0)
    {
        cJSON *root = cJSON_Parse(body);

        if (root == NULL)
        {
            return reply(req, "400 Bad Request", "{\"error\":\"invalid JSON\"}");
        }

        force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "force"));

        const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "path");

        if (cJSON_IsString(p) && p->valuestring != NULL)
        {
            if (strlen(p->valuestring) >= sizeof(path))
            {
                cJSON_Delete(root);
                return reply(req, "400 Bad Request", "{\"error\":\"path too long\"}");
            }

            strlcpy(path, p->valuestring, sizeof(path));
        }

        cJSON_Delete(root);
    }

    if (path[0] != '\0' && !filesystem_exists(path))
    {
        return reply(req, "404 Not Found", "{\"error\":\"no such file\"}");
    }

    esp_err_t err = obd_chip_firmware_update_start(
        (path[0] != '\0') ? path : NULL, force);

    if (err == ESP_ERR_INVALID_STATE)
    {
        return reply(req, "409 Conflict", "{\"error\":\"update already running\"}");
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "update start refused (%s)", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error",
                     "{\"error\":\"could not start the update\"}");
    }

    char out[160];

    snprintf(out, sizeof(out),
             "{\"started\":true,\"source\":\"%s\",\"force\":%s}",
             (path[0] != '\0') ? path : "builtin", force ? "true" : "false");
    return reply(req, "202 Accepted", out);
}

esp_err_t obd_chip_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/obd_chip", .method = HTTP_GET,
          .handler = obd_chip_get_handler },
        { .uri = "/api/obd_chip/update", .method = HTTP_POST,
          .handler = obd_chip_update_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/obd_chip routes registered");
    }

    return err;
}
