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
 * POST   /api/autopid/vehicles/detect          the detection job (202/409);
 *                                              an optional body names the
 *                                              car's profile and arms the
 *                                              job's probe (2026-10-26)
 * PUT    /api/autopid/vehicles/<key>           {name?, profile?, specific_init?}
 * POST   /api/autopid/vehicles/<key>/activate  switch by hand
 * POST   /api/autopid/vehicles/<key>/merge     {"into"}: fold <key> into
 *                                              another entry (2026-10-26)
 * DELETE /api/autopid/vehicles/<key>           forget the car (204)
 *
 * The `<key>` segment is percent-decoded before the lookup (2026-10-26,
 * discussion #1018): the web UI sends it through encodeURIComponent, so a
 * car without a readable VIN, keyed `fp:<8 hex>`, arrived as `fp%3A...`,
 * matched nothing, and could be neither forgotten nor given a profile.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "obd_chip_guard.h"

#include "autopid_private.h"
#include "autopid_http_private.h"

#define VEH_URI_PREFIX "/api/autopid/vehicles/"
#define VEH_BODY_MAX   512

static bool body_str(const cJSON *root, const char *name, size_t cap,
                     const char **out);

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

    /* the browser percent-encodes the key (fp:... arrives as fp%3A...):
       decode it, then validate the result like any key */
    if (n == 0 || !ap_vidx_key_decode(p, n, key, cap))
    {
        return false;
    }

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

/** The request body into @p buf (NUL-terminated): false when it does not
 *  fit or the read failed; no body = true with @p len 0. */
static bool read_body(httpd_req_t *req, char *buf, size_t cap, size_t *len)
{
    size_t want = req->content_len;
    size_t got = 0;

    *len = 0;
    buf[0] = '\0';

    if (want == 0)
    {
        return true;
    }

    if (want >= cap)
    {
        return false;
    }

    while (got < want)
    {
        int r = httpd_req_recv(req, buf + got, want - got);

        if (r <= 0)
        {
            return false;
        }

        got += (size_t)r;
    }

    buf[want] = '\0';
    *len = want;
    return true;
}

/** A string field of a body object into @p dst: absent = "" (fine);
 *  false when it is not a string or too long for @p cap. */
static bool take_str(const cJSON *root, const char *name, char *dst,
                     size_t cap)
{
    const char *v = NULL;

    dst[0] = '\0';

    if (!body_str(root, name, cap, &v))
    {
        return false;
    }

    if (v != NULL)
    {
        snprintf(dst, cap, "%s", v);
    }

    return true;
}

static bool hex_cmd(const char *s)
{
    size_t n = strlen(s);

    if (n < 2 || n >= AP_PROBE_CMD_LEN)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F') ||
              (s[i] >= 'a' && s[i] <= 'f')))
        {
            return false;
        }
    }

    return true;
}

/** The detect body (2026-10-26, the Quick Setup's "Which car is this?"):
 *  `{"profile","name","protocol","init","cmds":[...]}`, every field
 *  optional; arms the job's probe (autopid_scan_probe.c). False with
 *  @p bad set on a malformed body. An empty object clears the probe. */
static bool arm_probe(const char *json, const char **bad)
{
    static ap_scan_probe_t probe EXT_RAM_BSS_ATTR; /* httpd task only   */
    cJSON *root = cJSON_Parse(json);

    *bad = NULL;

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        *bad = "invalid json";
        return false;
    }

    memset(&probe, 0, sizeof(probe));

    char proto[4];

    if (!take_str(root, "profile", probe.profile, sizeof(probe.profile)) ||
        !take_str(root, "name", probe.name, sizeof(probe.name)) ||
        !take_str(root, "init", probe.init, sizeof(probe.init)) ||
        !take_str(root, "protocol", proto, sizeof(proto)))
    {
        cJSON_Delete(root);
        *bad = "profile/name/init/protocol: strings (63/31/95/1 chars max)";
        return false;
    }

    if (proto[0] != '\0' &&
        (proto[1] != '\0' ||
         !((proto[0] >= '1' && proto[0] <= '9') ||
           (proto[0] >= 'A' && proto[0] <= 'C'))))
    {
        cJSON_Delete(root);
        *bad = "protocol: one of 1..9, A..C";
        return false;
    }

    probe.protocol = proto[0];

    if (probe.init[0] != '\0' &&
        obd_chip_guard_check(probe.init, strlen(probe.init)) ==
            OBD_GUARD_BLOCKED)
    {
        cJSON_Delete(root);
        *bad = "init would write the chip's EEPROM (ATPP/ATSD/ATCV/STWBR)";
        return false;
    }

    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(root, "cmds");
    const cJSON *c = NULL;

    if (cmds != NULL && !cJSON_IsArray(cmds))
    {
        cJSON_Delete(root);
        *bad = "cmds: an array of up to 3 hex requests";
        return false;
    }

    cJSON_ArrayForEach(c, cmds)
    {
        /* "2101", or {"cmd":"2101","init":"ATSH7E4"} for a profile that
           addresses one ECU per request */
        const cJSON *cmd = cJSON_IsObject(c)
                               ? cJSON_GetObjectItemCaseSensitive(c, "cmd")
                               : c;
        const cJSON *init = cJSON_IsObject(c)
                                ? cJSON_GetObjectItemCaseSensitive(c, "init")
                                : NULL;

        if (!cJSON_IsString(cmd) || cmd->valuestring == NULL ||
            !hex_cmd(cmd->valuestring))
        {
            cJSON_Delete(root);
            *bad = "cmds: hex requests of 2 to 15 characters";
            return false;
        }

        if (init != NULL && !cJSON_IsNull(init) &&
            (!cJSON_IsString(init) || init->valuestring == NULL ||
             strlen(init->valuestring) >= AP_VEH_INIT_LEN ||
             obd_chip_guard_check(init->valuestring,
                                  strlen(init->valuestring)) ==
                 OBD_GUARD_BLOCKED))
        {
            cJSON_Delete(root);
            *bad = "cmds: a request's init is a string of 95 chars max "
                   "that does not write the chip's EEPROM";
            return false;
        }

        if (probe.n_cmds < AP_PROBE_CMDS_MAX)
        {
            snprintf(probe.cmds[probe.n_cmds], AP_PROBE_CMD_LEN, "%s",
                     cmd->valuestring);

            if (cJSON_IsString(init))
            {
                snprintf(probe.cmd_inits[probe.n_cmds], AP_VEH_INIT_LEN, "%s",
                         init->valuestring);
            }

            probe.n_cmds++;
        }
    }

    cJSON_Delete(root);

    if (probe.profile[0] == '\0' && probe.name[0] == '\0' &&
        probe.n_cmds == 0)
    {
        ap_scan_probe_clear();          /* `{}`: as before 2026-10-26     */
    }
    else
    {
        ap_scan_probe_set(&probe);
    }

    return true;
}

/** POST .../<key>/merge {"into":"<key>"}: "Same car as..." in the list. */
static esp_err_t merge_handler(httpd_req_t *req, const char *key)
{
    char body[VEH_BODY_MAX + 1];
    size_t len = 0;
    char into[AP_VEH_KEY_LEN];

    if (!read_body(req, body, sizeof(body), &len) || len == 0)
    {
        return ap_http_send_error(req, "400 Bad Request",
                                  "missing/oversized body");
    }

    cJSON *root = cJSON_Parse(body);

    if (!cJSON_IsObject(root) ||
        !take_str(root, "into", into, sizeof(into)) || into[0] == '\0' ||
        !ap_vidx_key_valid(into))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                                  "into: the key of the car to keep");
    }

    cJSON_Delete(root);

    esp_err_t err = autopid_vehicle_merge(key, into);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ap_http_send_error(req, "404 Not Found", "no such vehicle");
    }

    if (err == ESP_ERR_INVALID_ARG)
    {
        return ap_http_send_error(req, "400 Bad Request",
                                  "into: another car than this one");
    }

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                                  "merge failed");
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
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
        /* an optional body (2026-10-26, the Quick Setup's "Which car is
           this?"): the profile the user named, the entry's name, the
           protocol its init pins and up to three of its own requests, the
           job's probe when nothing standard answers; `{}` or no body =
           the plain job, as before */
        char body[VEH_BODY_MAX + 1];
        size_t blen = 0;
        const char *bad = NULL;

        if (!read_body(req, body, sizeof(body), &blen))
        {
            return ap_http_send_error(req, "400 Bad Request",
                                      "oversized body / body read failed");
        }

        if (blen > 0)
        {
            if (!arm_probe(body, &bad))
            {
                return ap_http_send_error(req, "400 Bad Request", bad);
            }
        }
        else
        {
            ap_scan_probe_clear();
        }

        esp_err_t err = autopid_std_scan_start();

        if (err != ESP_OK)
        {
            ap_scan_probe_clear();
        }

        if (err == ESP_ERR_INVALID_STATE)
        {
            return ap_http_send_error(req, "409 Conflict",
                                      "detection already running");
        }

        if (err == ESP_ERR_NOT_SUPPORTED)
        {
            return ap_http_send_guard_error(req);
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

    if (!uri_key(req, key, sizeof(key), &rest))
    {
        return ap_http_send_error(req, "404 Not Found", "no such route");
    }

    if (strcmp(rest, "/merge") == 0)
    {
        return merge_handler(req, key);
    }

    if (strcmp(rest, "/activate") != 0)
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
