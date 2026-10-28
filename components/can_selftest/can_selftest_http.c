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
 * @file can_selftest_http.c
 * @brief POST /api/can/selftest starts a run (202; 409 while one runs),
 *        GET /api/can/selftest serves the result, running or done
 *        (own-routes pattern, §9.1; main wires it). Hand-formatted JSON.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "can_selftest.h"

static const char *TAG = "can_selftest";

static const char *const REPLY_NAMES[] = { "none", "ours", "other", "no_data",
                                           "can_error", "error" };

/** JSON-safe: the quote and the backslash become a dot (the texts are the
 *  chip's own ASCII and ours; nothing else needs escaping). */
static void json_safe(char *out, size_t out_len, const char *in)
{
    size_t i;

    for (i = 0; in[i] != '\0' && i + 1 < out_len; i++)
    {
        out[i] = (in[i] == '"' || in[i] == '\\' || (unsigned char)in[i] < 32)
                     ? '.' : in[i];
    }

    out[i] = '\0';
}

static esp_err_t selftest_get_handler(httpd_req_t *req)
{
    static can_selftest_result_t s_copy EXT_RAM_BSS_ATTR;
    const size_t cap = 4096;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (body == NULL)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no memory\"}", HTTPD_RESP_USE_STRLEN);
    }

    can_selftest_last(&s_copy);

    char text[CAN_SELFTEST_LINE + 8];
    size_t n = 0;

    json_safe(text, sizeof(text), s_copy.verdict ? s_copy.verdict : "");
    n += (size_t)snprintf(body + n, cap - n,
                          "{\"running\":%s,\"done\":%s,\"pass\":%s,"
                          "\"verdict\":\"%s\",\"refusal\":\"%s\","
                          "\"bus\":{\"baud_kbps\":%lu,\"live\":%s},"
                          "\"esp_heard\":%s,\"reply\":\"%s\",\"rpm\":%d,"
                          "\"took_ms\":%lu,\"steps\":[",
                          s_copy.running ? "true" : "false",
                          s_copy.done ? "true" : "false",
                          s_copy.pass ? "true" : "false", text,
                          s_copy.refusal, (unsigned long)s_copy.baud_kbps,
                          s_copy.live ? "true" : "false",
                          s_copy.esp_heard ? "true" : "false",
                          REPLY_NAMES[(s_copy.reply <= CAN_SELFTEST_REPLY_ERROR)
                                          ? s_copy.reply : 0],
                          s_copy.rpm, (unsigned long)s_copy.took_ms);

    for (size_t i = 0; s_copy.done && i < CAN_SELFTEST_STEPS && n + 300 < cap; i++)
    {
        char detail[sizeof(s_copy.steps[i].detail) + 8];

        json_safe(detail, sizeof(detail), s_copy.steps[i].detail);
        n += (size_t)snprintf(body + n, cap - n,
                              "%s{\"name\":\"%s\",\"ok\":%s,\"ms\":%lu,"
                              "\"detail\":\"%s\"}",
                              (i > 0) ? "," : "",
                              s_copy.steps[i].name ? s_copy.steps[i].name : "",
                              s_copy.steps[i].ok ? "true" : "false",
                              (unsigned long)s_copy.steps[i].ms, detail);
    }

    n += (size_t)snprintf(body + n, cap - n, "],\"lines\":[");

    for (size_t i = 0; i < s_copy.n_lines && n + 120 < cap; i++)
    {
        json_safe(text, sizeof(text), s_copy.lines[i]);
        n += (size_t)snprintf(body + n, cap - n, "%s\"%s\"",
                              (i > 0) ? "," : "", text);
    }

    n += (size_t)snprintf(body + n, cap - n, "]}");

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, (ssize_t)n);

    heap_caps_free(body);
    return err;
}

static esp_err_t selftest_post_handler(httpd_req_t *req)
{
    esp_err_t err = can_selftest_run_start();

    httpd_resp_set_type(req, "application/json");

    if (err == ESP_ERR_INVALID_STATE)
    {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "{\"error\":\"a run is in progress\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "run start refused (%s)", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"error\":\"could not start the test\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, "{\"started\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t can_selftest_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/can/selftest", .method = HTTP_GET,
          .handler = selftest_get_handler },
        { .uri = "/api/can/selftest", .method = HTTP_POST,
          .handler = selftest_post_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/can/selftest routes registered");
    }

    return err;
}
