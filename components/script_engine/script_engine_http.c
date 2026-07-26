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
 * @file script_engine_http.c
 * @brief The /api/scripts surface: GET /api/scripts (list stored
 *        scripts), POST /api/scripts/run {src|name} (inline source or a
 *        stored /data/scripts/<name>.be), POST /api/scripts/stop (kill
 *        switch). File CRUD rides the generic /api/fs surface
 *        (upload?path=/data/scripts/x.be, download, DELETE file).
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "filesystem.h"
#include "http_server_manager.h"

#include "script_engine.h"
#include "script_engine_private.h"

static const char *TAG = "script_engine";

#define SE_SRC_MAX  8192
#define SE_OUT_MAX  4096

static esp_err_t run_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > SE_SRC_MAX)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/big body");
        return ESP_FAIL;
    }

    char *body = malloc(len + 1);

    if (body == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "src");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    bool by_name = cJSON_IsString(name) && name->valuestring[0] != '\0';

    if (!by_name &&
        (!cJSON_IsString(src) || src->valuestring[0] == '\0'))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need \"src\" or \"name\"");
        return ESP_FAIL;
    }

    char *out = heap_caps_malloc(SE_OUT_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (out == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    out[0] = '\0';
    esp_err_t err = by_name
        ? script_engine_run_file(name->valuestring, out, SE_OUT_MAX)
        : script_engine_run(src->valuestring, out, SE_OUT_MAX);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG ||
        err == ESP_ERR_INVALID_SIZE)
    {
        free(out);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "no such script (or bad name/size)");
        return ESP_FAIL;
    }

    cJSON *o = cJSON_CreateObject();

    if (err == ESP_ERR_INVALID_STATE)
    {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error",
                                "busy or script_engine disabled");
        httpd_resp_set_status(req, "409 Conflict");
    }
    else
    {
        cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
        cJSON_AddStringToObject(o, "output", out);
    }

    free(out);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    if (s == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

/* GET /api/scripts — enumerate the .be files in /data/scripts for the
 * UI (CRUD via the generic /api/fs surface: upload?path=… etc.). */
static esp_err_t list_cb(const char *name, bool is_dir, size_t size,
                         void *ctx)
{
    if (is_dir)
    {
        return ESP_OK;
    }

    size_t n = strlen(name);

    if (n < 4 || strcmp(name + n - 3, ".be") != 0)
    {
        return ESP_OK;
    }

    cJSON *e = cJSON_CreateObject();

    cJSON_AddStringToObject(e, "name", name);
    cJSON_AddNumberToObject(e, "size", (double)size);
    cJSON_AddItemToArray((cJSON *)ctx, e);
    return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "scripts");

    (void)filesystem_list(SE_SCRIPTS_DIR, list_cb, arr); /* absent dir = empty */
    cJSON_AddStringToObject(o, "dir", SE_SCRIPTS_DIR);
    cJSON_AddBoolToObject(o, "busy", script_engine_busy());

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    if (s == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    script_engine_kill();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t script_engine_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/scripts", .method = HTTP_GET,
          .handler = list_handler },
        { .uri = "/api/scripts/run", .method = HTTP_POST,
          .handler = run_handler },
        { .uri = "/api/scripts/stop", .method = HTTP_POST,
          .handler = stop_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/scripts routes registered");
    }

    return err;
}
