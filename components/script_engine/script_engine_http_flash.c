/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_http_flash.c
 * @brief The web side of long runs and ECU flashing:
 *          POST /api/scripts/run {..., "async": true[, "params": {}]}
 *               -> 202 {ok, async, run}; the run goes on without the
 *               request. 400 no or bad source, 404 no such script, 413
 *               over 64 KB, 503 no SD card, 409 busy or disabled
 *          GET  /api/scripts/status[?since=n]
 *               -> lock, running, phase, progress, ecu_state, prompt,
 *                  result, log lines after n, next, output
 *          POST /api/scripts/answer {"value": "..."} -> the open prompt
 *          POST /api/scripts/release {"ack": true}
 *               -> leave a failed (or restart-interrupted) flash's lock;
 *               409 nothing held, 500 the record was not cleared (held)
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"

#include "http_server_manager.h"

#include "script_engine_doc.h"
#include "script_engine_flash.h"
#include "script_engine_private.h"

#define SE_FILE_SCRIPT_MAX (64 * 1024) /* a script on the SD card          */

static esp_err_t reply(httpd_req_t *req, const char *status, cJSON *o)
{
    char *s = (o != NULL) ? cJSON_PrintUnformatted(o) : NULL;

    cJSON_Delete(o);

    if (s == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    if (status != NULL)
    {
        httpd_resp_set_status(req, status);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *error)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", error);
    return reply(req, status, o);
}

/* A small JSON body (answer, release); NULL after sending the error. */
static cJSON *small_body(httpd_req_t *req)
{
    char buf[256];
    size_t len = req->content_len;

    if (len == 0 || len >= sizeof(buf))
    {
        fail(req, "400 Bad Request", "missing or big body");
        return NULL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, buf + got, len - got);

        if (r <= 0)
        {
            fail(req, "400 Bad Request", "recv");
            return NULL;
        }

        got += (size_t)r;
    }

    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);

    if (root == NULL)
    {
        fail(req, "400 Bad Request", "invalid json");
    }

    return root;
}

/* A script on the SD card into a fresh PSRAM buffer. */
static esp_err_t load_sd(const char *path, char **out)
{
    size_t size = 0;

    if (!se_file_path_ok(path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = se_file_size(path, &size);

    if (err != ESP_OK)
    {
        return err;
    }

    if (size == 0 || size > SE_FILE_SCRIPT_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char *src = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t got = 0;

    if (src == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    err = se_file_read(path, 0, (uint8_t *)src, size, &got);
    src[got] = '\0';

    if (err != ESP_OK)
    {
        free(src);
        return err;
    }

    *out = src;
    return ESP_OK;
}

esp_err_t se_http_run_async(void *vreq, cJSON *root)
{
    httpd_req_t *req = vreq;
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "src");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "path");
    char *code = NULL;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool have = true;

    if (cJSON_IsString(src) && src->valuestring[0] != '\0')
    {
        size_t n = strlen(src->valuestring);

        code = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        err = (code != NULL) ? ESP_OK : ESP_ERR_NO_MEM;

        if (code != NULL)
        {
            memcpy(code, src->valuestring, n + 1);
        }
    }
    else if (cJSON_IsString(name) && name->valuestring[0] != '\0')
    {
        err = se_load_stored(name->valuestring, &code);
    }
    else if (cJSON_IsString(path) && path->valuestring[0] != '\0')
    {
        err = load_sd(path->valuestring, &code);
    }
    else
    {
        have = false;
    }

    cJSON *params = cJSON_DetachItemFromObjectCaseSensitive(root, "params");

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        cJSON_Delete(params);
        free(code);

        if (!have)
        {
            return fail(req, "400 Bad Request",
                        "need \"src\", a stored \"name\" or an SD \"path\"");
        }

        switch (err)
        {
            case ESP_ERR_INVALID_ARG:
                return fail(req, "400 Bad Request",
                            "bad name, or a path outside /sd/ (no \"..\")");
            case ESP_ERR_NOT_FOUND:
                return fail(req, "404 Not Found", "no such script");
            case ESP_ERR_INVALID_SIZE:
                return fail(req, "413 Payload Too Large",
                            "the script is empty or over 64 KB");
            case ESP_ERR_INVALID_STATE:
                return fail(req, "503 Service Unavailable", "no SD card mounted");
            case ESP_ERR_NO_MEM:
                return fail(req, "500 Internal Server Error", "oom");
            default:
                return fail(req, "500 Internal Server Error", esp_err_to_name(err));
        }
    }

    if (params != NULL && !cJSON_IsObject(params))
    {
        cJSON_Delete(params);
        params = NULL;
    }

    uint32_t run = 0;

    err = script_engine_run_async(code, params, &run); /* takes both */

    if (err != ESP_OK)
    {
        return fail(req, "409 Conflict", "busy or script_engine disabled");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "async", true);
    cJSON_AddNumberToObject(o, "run", run);
    return reply(req, "202 Accepted", o);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char query[48];
    char since[16];
    uint32_t n = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", since, sizeof(since)) == ESP_OK)
    {
        n = (uint32_t)strtoul(since, NULL, 10);
    }

    cJSON *o = se_flash_status(n);

    if (o != NULL)
    {
        cJSON_AddBoolToObject(o, "allow_reflash", se_settings_allow_reflash());
        cJSON_AddStringToObject(o, "output", se_async_output());
    }

    return reply(req, NULL, o);
}

static esp_err_t answer_handler(httpd_req_t *req)
{
    cJSON *root = small_body(req);

    if (root == NULL)
    {
        return ESP_FAIL;
    }

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
    esp_err_t err = se_flash_answer(cJSON_IsString(v) ? v->valuestring : "");

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        return fail(req, "409 Conflict", "no open prompt");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return reply(req, NULL, o);
}

static esp_err_t release_handler(httpd_req_t *req)
{
    cJSON *root = small_body(req);

    if (root == NULL)
    {
        return ESP_FAIL;
    }

    bool ack = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ack"));

    cJSON_Delete(root);

    if (!ack)
    {
        return fail(req, "400 Bad Request",
                    "{\"ack\": true} confirms the ECU may not start");
    }

    esp_err_t err = se_flash_release();

    if (err == ESP_ERR_INVALID_STATE)
    {
        return fail(req, "409 Conflict",
                    "nothing to release (a running flash is never released)");
    }

    if (err != ESP_OK)
    {
        return fail(req, "500 Internal Server Error",
                    "the record was not cleared: still locked");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return reply(req, NULL, o);
}

esp_err_t se_flash_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/scripts/status", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/scripts/answer", .method = HTTP_POST,
          .handler = answer_handler },
        { .uri = "/api/scripts/release", .method = HTTP_POST,
          .handler = release_handler },
    };

    return http_server_manager_register_handlers(URIS,
                                                 sizeof(URIS) / sizeof(URIS[0]));
}
