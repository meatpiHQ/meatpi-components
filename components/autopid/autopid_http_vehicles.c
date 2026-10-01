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
 * @file autopid_http_vehicles.c
 * @brief The vehicle store routes (TASK_quick_setup.md second pass;
 *        HTTP_API.md 6e4). The route table stays in autopid_http.c; the
 *        three per-key methods share one wildcard URI each (the
 *        `vehicles/` prefix plus the httpd wildcard) and dispatch on the
 *        path here, so `detect` and `<key>/activate` never depend on
 *        registration order. httpd-task context: internal stack, the
 *        store's file work runs right here.
 *
 * GET    /api/autopid/vehicles                 the index (+ "current")
 * POST   /api/autopid/vehicles/detect          the detection job (202/409)
 * PUT    /api/autopid/vehicles/<key>           {name?, profile?, specific_init?}
 * POST   /api/autopid/vehicles/<key>/activate  switch by hand
 * DELETE /api/autopid/vehicles/<key>           forget the car (204)
 */
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "obd_chip_guard.h"

#include "autopid_private.h"
#include "autopid_http_private.h"

#define VEH_URI_PREFIX "/api/autopid/vehicles/"
#define VEH_BODY_MAX   512

/** The `<key>` segment after the prefix and what follows it (`"/..."` or
 *  `""`, the query string stripped). False when the key is malformed. */
static bool uri_key(httpd_req_t *req, char *key, size_t cap,
                    const char **rest)
{
    size_t plen = strlen(VEH_URI_PREFIX);

    *rest = "";

    if (strncmp(req->uri, VEH_URI_PREFIX, plen) != 0)
    {
        return false;
    }

    const char *p = req->uri + plen;
    size_t n = 0;

    while (p[n] != '\0' && p[n] != '/' && p[n] != '?')
    {
        n++;
    }

    if (n == 0 || n >= cap)
    {
        return false;
    }

    memcpy(key, p, n);
    key[n] = '\0';

    static char s_rest[32];         /* httpd task only                  */
    size_t r = 0;

    for (const char *q = p + n; *q != '\0' && *q != '?' &&
                                r < sizeof(s_rest) - 1; q++)
    {
        s_rest[r++] = *q;
    }

    s_rest[r] = '\0';
    *rest = s_rest;
    return ap_vidx_key_valid(key);
}

static esp_err_t send_entry(httpd_req_t *req, const char *key)
{
    cJSON *o = autopid_vehicle_entry_json(key);

    if (o == NULL)
    {
        return ap_http_send_error(req, "404 Not Found", "no such vehicle");
    }

    return ap_http_send_json(req, o);
}

esp_err_t vehicles_get_handler(httpd_req_t *req)
{
    cJSON *o = autopid_vehicles_json();

    if (o == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    return ap_http_send_json(req, o);
}

esp_err_t vehicles_post_handler(httpd_req_t *req)
{
    char key[AP_VEH_KEY_LEN];
    const char *rest = "";

    /* POST .../detect = the detection job (the std scan job grown into
       vehicle detection: protocol, VIN, responders, standard PIDs) */
    if (strncmp(req->uri, VEH_URI_PREFIX "detect", strlen(VEH_URI_PREFIX) + 6)
            == 0 &&
        (req->uri[strlen(VEH_URI_PREFIX) + 6] == '\0' ||
         req->uri[strlen(VEH_URI_PREFIX) + 6] == '?'))
    {
        esp_err_t err = autopid_std_scan_start();

        if (err == ESP_ERR_INVALID_STATE)
        {
            return ap_http_send_error(req, "409 Conflict",
                                      "detection already running");
        }

        if (err != ESP_OK)
        {
            return ap_http_send_error(req, "500 Internal Server Error",
                                      "detection start failed");
        }

        httpd_resp_set_status(req, "202 Accepted");

        cJSON *o = cJSON_CreateObject();

        cJSON_AddBoolToObject(o, "started", true);
        return ap_http_send_json(req, o);
    }

    if (!uri_key(req, key, sizeof(key), &rest) ||
        strcmp(rest, "/activate") != 0)
    {
        return ap_http_send_error(req, "404 Not Found", "no such route");
    }

    esp_err_t err = autopid_vehicle_activate(key);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ap_http_send_error(req, "404 Not Found", "no such vehicle");
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                                  "activate failed");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    return ap_http_send_json(req, o);
}

/** A string field of the PUT body: NULL when absent, "" when null/empty;
 *  false (400) when it is not a string or too long for its slot. */
static bool body_str(const cJSON *root, const char *name, size_t cap,
                     const char **out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);

    *out = NULL;

    if (v == NULL)
    {
        return true;
    }

    if (cJSON_IsNull(v))
    {
        *out = "";
        return true;
    }

    if (!cJSON_IsString(v) || v->valuestring == NULL ||
        strlen(v->valuestring) >= cap)
    {
        return false;
    }

    *out = v->valuestring;
    return true;
}

esp_err_t vehicles_put_handler(httpd_req_t *req)
{
    char key[AP_VEH_KEY_LEN];
    const char *rest = "";
    char body[VEH_BODY_MAX + 1];
    size_t len = req->content_len;

    if (!uri_key(req, key, sizeof(key), &rest) || rest[0] != '\0')
    {
        return ap_http_send_error(req, "404 Not Found", "no such route");
    }

    if (len == 0 || len > VEH_BODY_MAX)
    {
        return ap_http_send_error(req, "400 Bad Request",
                                  "missing/oversized body");
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            return ap_http_send_error(req, "400 Bad Request",
                                      "body read failed");
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request", "invalid json");
    }

    const char *name = NULL, *profile = NULL, *init = NULL;

    if (!body_str(root, "name", AP_VEH_NAME_LEN, &name) ||
        !body_str(root, "profile", AP_VEH_PROFILE_LEN, &profile) ||
        !body_str(root, "specific_init", AP_INIT_LEN, &init))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                                  "name/profile/specific_init: strings "
                                  "(31/63/95 chars max)");
    }

    /* the same EEPROM rule as the config tables: an init that would
       write the chip's EEPROM is refused (ATSP is rewritten at send) */
    if (init != NULL &&
        obd_chip_guard_check(init, strlen(init)) == OBD_GUARD_BLOCKED)
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                                  "specific_init would write the chip's "
                                  "EEPROM (ATPP/ATSD/ATCV/STWBR)");
    }

    esp_err_t err = autopid_vehicle_update(key, name, profile, init);

    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ap_http_send_error(req, "404 Not Found", "no such vehicle");
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                                  "store failed");
    }

    return send_entry(req, key);
}

esp_err_t vehicles_delete_handler(httpd_req_t *req)
{
    char key[AP_VEH_KEY_LEN];
    const char *rest = "";

    if (!uri_key(req, key, sizeof(key), &rest) || rest[0] != '\0')
    {
        return ap_http_send_error(req, "404 Not Found", "no such route");
    }

    esp_err_t err = autopid_vehicle_delete(key);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ap_http_send_error(req, "404 Not Found", "no such vehicle");
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                                  "delete failed");
    }

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
