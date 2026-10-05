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
 * @file autopid_http_dbc.c
 * @brief The DBC routes of autopid (files, signals, add-as-filter,
 *        HTTP_API.md 6e4c): handlers only, the route table stays in
 *        autopid_http.c.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "battery_monitor.h"
#include "filesystem.h"
#include "http_server_manager.h"

#include "expression_parser.h"

#include "autopid.h"
#include "autopid_private.h"
#include "autopid_http_private.h"

static const char *TAG = "autopid";

/* ---- DBC files (TASK_dbc.md §5; HTTP_API.md §6e4c) ----------------------- */

esp_err_t dbc_get_handler(httpd_req_t *req)
{
    cJSON *o = ap_dbc_list_json();

    return (o != NULL) ? ap_http_send_json(req, o)
                       : ap_http_send_error(req, "500 Internal Server Error",
                                    "oom");
}

esp_err_t dbc_post_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!ap_http_query_param(req, "name", name, sizeof(name)))
    {
        return ap_http_send_error(req, "400 Bad Request", "?name=<db> required");
    }

    size_t len = req->content_len;

    if (len == 0 || len > AP_DBC_FILE_MAX)
    {
        return ap_http_send_error(req, "400 Bad Request",
                          "body 1..1048576 bytes (raw .dbc)");
    }

    char *raw = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);

    if (raw == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, raw + got, len - got);

        if (r <= 0)
        {
            heap_caps_free(raw);
            return ap_http_send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    raw[len] = '\0';

    int n_msgs = 0, n_sigs = 0;
    char err_text[96] = "";
    esp_err_t err = ap_dbc_store(name, raw, len, &n_msgs, &n_sigs,
                                 err_text, sizeof(err_text));

    heap_caps_free(raw);

    if (err == ESP_ERR_NO_MEM && err_text[0] == 'a')  /* slots full */
    {
        return ap_http_send_error(req, "409 Conflict", err_text);
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "400 Bad Request",
                          err_text[0] ? err_text : "import failed");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "messages", n_msgs);
    cJSON_AddNumberToObject(o, "signals", n_sigs);
    ESP_LOGI(TAG, "dbc '%s' uploaded: %d msgs / %d signals", name,
             n_msgs, n_sigs);
    return ap_http_send_json(req, o);
}

esp_err_t dbc_delete_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!ap_http_query_param(req, "name", name, sizeof(name)))
    {
        return ap_http_send_error(req, "400 Bad Request", "?name=<db> required");
    }

    if (ap_dbc_delete(name) != ESP_OK)
    {
        return ap_http_send_error(req, "404 Not Found", "no such DBC");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return ap_http_send_json(req, o);
}

esp_err_t dbc_signals_handler(httpd_req_t *req)
{
    char q[64] = "", db[AP_DTC_DB_NAME_LEN] = "", num[12];
    int offset = 0, limit = 50;

    (void)ap_http_query_param(req, "q", q, sizeof(q));
    (void)ap_http_query_param(req, "db", db, sizeof(db));

    if (ap_http_query_param(req, "offset", num, sizeof(num)))
    {
        offset = atoi(num);
    }

    if (ap_http_query_param(req, "limit", num, sizeof(num)))
    {
        limit = atoi(num);
    }

    if (offset < 0 || limit < 1 || limit > 100)
    {
        return ap_http_send_error(req, "400 Bad Request",
                          "offset>=0, 1<=limit<=100");
    }

    cJSON *o = ap_dbc_signals_json(db, q, offset, limit);

    return (o != NULL) ? ap_http_send_json(req, o)
                       : ap_http_send_error(req, "500 Internal Server Error",
                                    "oom");
}

esp_err_t dbc_add_handler(httpd_req_t *req)
{
    /* heap, not stack: 2 KB of locals on the httpd task is how the
     * 2026-07-08 heap-corruption hunt started */
    char *body = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);

    if (body == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    int len = httpd_req_recv(req, body, 2047);

    if (len <= 0)
    {
        heap_caps_free(body);
        return ap_http_send_error(req, "400 Bad Request", "missing body");
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);

    heap_caps_free(body);
    const cJSON *db = cJSON_GetObjectItemCaseSensitive(root, "db");
    const cJSON *signals = cJSON_GetObjectItemCaseSensitive(root,
                                                            "signals");
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group");
    const cJSON *mon = cJSON_GetObjectItemCaseSensitive(root,
                                                        "monitor_ms");
    const cJSON *per = cJSON_GetObjectItemCaseSensitive(root,
                                                        "period_ms");

    if (!cJSON_IsString(db) || !cJSON_IsArray(signals) ||
        cJSON_GetArraySize(signals) == 0)
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                          "need \"db\" + non-empty \"signals\" array");
    }

    cJSON *result = NULL;
    char err_text[128] = "";
    esp_err_t err = ap_dbc_add(
        db->valuestring, signals,
        cJSON_IsString(group) ? group->valuestring : NULL,
        cJSON_IsNumber(mon) ? mon->valueint : 0,
        cJSON_IsNumber(per) ? per->valueint : 0, &result, err_text,
        sizeof(err_text));

    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ap_http_send_error(req, "404 Not Found", err_text);
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return ap_http_send_error(req, "400 Bad Request", err_text);
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                          err_text[0] ? err_text : "add failed");
    }

    return ap_http_send_json(req, result);
}

/* POST /api/autopid/group {"name":"...","enabled":bool[,"period_ms":N]}
 * Runtime (non-persisted) group toggle: the HTTP twin of the
 * `autopid.group` event action (API-first §1b: anything a rule can do,
 * a client can do). Used by the bench to claim OBD exclusivity. */
