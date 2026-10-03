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
 * @file autopid_http_dtc.c
 * @brief The DTC routes of autopid (check / report / clear and the code
 *        databases, HTTP_API.md 6e4b): handlers only, the route table
 *        stays in autopid_http.c.
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

/* ---- DTC (TASK_dtc.md §8; HTTP_API.md §6e4b) ---------------------------- */

esp_err_t dtc_get_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    /* with the databases' descriptions: {"desc":{"P0420":"Catalyst …"}}
       for every report code with a hit */
    cJSON *report = ap_dtc_report_json_desc();

    if (o == NULL || report == NULL)
    {
        cJSON_Delete(o);
        cJSON_Delete(report);
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    cJSON_AddBoolToObject(o, "enabled", ap_dtc_enabled());
    cJSON_AddBoolToObject(o, "allow_clear", ap_dtc_clear_gate_open());
    cJSON_AddBoolToObject(o, "scanning", ap_dtc_busy());
    cJSON_AddStringToObject(o, "path", ap_dtc_path());
    cJSON_AddItemToObject(o, "report", report);
    return ap_http_send_json(req, o);
}

/* ---- DTC databases (TASK_dtc_db.md §4) ---------------------------------- */

/** ?key=value from the query string into @p out; false when absent. */
bool ap_http_query_param(httpd_req_t *req, const char *key, char *out,
                        size_t out_cap)
{
    char qs[160];

    out[0] = '\0';

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK)
    {
        return false;
    }

    return httpd_query_key_value(qs, key, out, out_cap) == ESP_OK;
}

esp_err_t dtc_db_get_handler(httpd_req_t *req)
{
    cJSON *o = ap_dtc_db_list_json();

    if (o == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    return ap_http_send_json(req, o);
}

esp_err_t dtc_db_post_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!ap_http_query_param(req, "name", name, sizeof(name)))
    {
        return ap_http_send_error(req, "400 Bad Request", "?name=<db> required");
    }

    size_t len = req->content_len;

    if (len == 0 || len > AP_DTC_DB_FILE_MAX)
    {
        return ap_http_send_error(req, "400 Bad Request",
                          "body 1..1048576 bytes (raw database file)");
    }

    char *raw = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);

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

    int entries = 0;
    char fmt[12] = "";
    char err_text[96] = "";
    esp_err_t err = ap_dtc_db_store(name, raw, len, &entries, fmt,
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
    cJSON_AddNumberToObject(o, "entries", entries);
    cJSON_AddStringToObject(o, "format", fmt);
    ESP_LOGI(TAG, "dtc db '%s' uploaded: %d entries (%s)", name, entries,
             fmt);
    return ap_http_send_json(req, o);
}

esp_err_t dtc_db_delete_handler(httpd_req_t *req)
{
    char name[AP_DTC_DB_NAME_LEN];

    if (!ap_http_query_param(req, "name", name, sizeof(name)))
    {
        return ap_http_send_error(req, "400 Bad Request", "?name=<db> required");
    }

    if (ap_dtc_db_delete(name) != ESP_OK)
    {
        return ap_http_send_error(req, "404 Not Found", "no such database");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return ap_http_send_json(req, o);
}

esp_err_t dtc_db_search_handler(httpd_req_t *req)
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

    cJSON *o = ap_dtc_db_search_json(q, db, offset, limit);

    if (o == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    return ap_http_send_json(req, o);
}

esp_err_t dtc_lookup_handler(httpd_req_t *req)
{
    char codes[192];

    if (!ap_http_query_param(req, "codes", codes, sizeof(codes)))
    {
        return ap_http_send_error(req, "400 Bad Request",
                          "?codes=P0420,P0171 required");
    }

    cJSON *o = cJSON_CreateObject();
    char *p = codes;

    while (*p != '\0' && o != NULL)
    {
        char *sep = strchr(p, ',');

        if (sep != NULL)
        {
            *sep = '\0';
        }

        if (*p != '\0')
        {
            char desc[AP_DTC_DESC_MAX + 1];

            if (autopid_dtc_desc(p, desc, sizeof(desc)) == ESP_OK)
            {
                cJSON_AddStringToObject(o, p, desc);
            }
            else
            {
                cJSON_AddNullToObject(o, p);
            }
        }

        p = (sep != NULL) ? sep + 1 : p + strlen(p);
    }

    return ap_http_send_json(req, o);
}

esp_err_t dtc_scan_post_handler(httpd_req_t *req)
{
    esp_err_t err = ap_dtc_scan_start();

    if (err == ESP_ERR_NOT_ALLOWED)
    {
        return ap_http_send_error(req, "403 Forbidden", "dtc_enabled is off");
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return ap_http_send_error(req, "409 Conflict", "another chip job runs");
    }

    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        return ap_http_send_guard_error(req);
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                          "scan start failed");
    }

    httpd_resp_set_status(req, "202 Accepted");

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "started", true);
    return ap_http_send_json(req, o);
}

esp_err_t dtc_clear_post_handler(httpd_req_t *req)
{
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);

    if (len <= 0)
    {
        return ap_http_send_error(req, "400 Bad Request", "missing body");
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root,
                                                            "confirm");
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(root, "codes");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");

    if (!cJSON_IsTrue(confirm))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                          "confirm:true required — mode 04 clears ALL "
                          "codes + readiness monitors");
    }

    bool cleared = false;
    uint8_t before = 0, after = 0;
    char err_text[48] = "";
    esp_err_t err = ap_dtc_clear(
        cJSON_IsString(codes) ? codes->valuestring : NULL,
        cJSON_IsString(mode) ? mode->valuestring : NULL, &cleared,
        &before, &after, err_text, sizeof(err_text));

    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_ALLOWED)
    {
        return ap_http_send_error(req, "403 Forbidden", err_text);
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return ap_http_send_error(req, "400 Bad Request", err_text);
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        return ap_http_send_error(req, "409 Conflict", err_text);
    }

    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        /* refused by the bus guard: its whole sentence (err_text is too
           short for it) */
        return ap_http_send_guard_error(req);
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(o, "cleared", cleared);
    cJSON_AddNumberToObject(o, "before", before);
    cJSON_AddNumberToObject(o, "after", after);

    if (err != ESP_OK && err_text[0] != '\0')
    {
        cJSON_AddStringToObject(o, "error", err_text);
    }

    return ap_http_send_json(req, o);
}
