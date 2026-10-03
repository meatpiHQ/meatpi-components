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
 * @file autopid_http.c
 * @brief The /api/autopid routes (own-routes pattern, §9.1) — main calls
 *        autopid_register_http() only in HTTP compositions.
 *
 * GET  /api/autopid          — live cache + groups + stats (the dashboard)
 * GET  /api/autopid/data     — the LEGACY-shape {"Name": value} snapshot
 * GET  /api/autopid/config   — the PID/filter tables file, verbatim
 * PUT  /api/autopid/config   — validate -> atomic save -> LIVE reload
 *                              (the config FILE applies live; settings
 *                              knobs stay reboot-to-apply; the save also
 *                              lands in the current car's tables file)
 * The vehicle store routes (/api/autopid/vehicles*) live in
 * autopid_http_vehicles.c, DTC in autopid_http_dtc.c, DBC in
 * autopid_http_dbc.c; this file keeps the route table.
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
#include "j1939.h" /* j1939_active: the `?` rows' requests go out */

#include "expression_parser.h"

#include "autopid.h"
#include "autopid_private.h"
#include "autopid_http_private.h"

static const char *TAG = "autopid";

#define AP_HTTP_BODY_MAX (256 * 1024)

esp_err_t ap_http_send_json(httpd_req_t *req, cJSON *obj)
{
    char *body = cJSON_PrintUnformatted(obj);

    cJSON_Delete(obj);

    if (body == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "oom");
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr(req, body);

    free(body);
    return err;
}

esp_err_t ap_http_send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    httpd_resp_set_status(req, status);
    cJSON_AddStringToObject(o, "error", msg);
    return ap_http_send_json(req, o);
}

esp_err_t ap_http_send_guard_error(httpd_req_t *req)
{
    char reason[176];

    /* a chip job was refused by the bus guard: say why, in its words */
    ap_guard_last_reason(reason, sizeof(reason));
    return ap_http_send_error(req, "409 Conflict", reason);
}

static esp_err_t autopid_get_handler(httpd_req_t *req)
{
    autopid_stats_t st;

    autopid_stats(&st);

    cJSON *o = cJSON_CreateObject();
    cJSON *groups = cJSON_AddArrayToObject(o, "groups");

    ap_core_group_json(groups);
    cJSON_AddItemToObject(o, "params", ap_cache_detail(ap_core_config()));

    cJSON *stats = cJSON_AddObjectToObject(o, "stats");

    cJSON_AddBoolToObject(stats, "running", st.running);
    cJSON_AddBoolToObject(stats, "paused_voltage", st.paused_voltage);
    cJSON_AddBoolToObject(stats, "paused_client", st.paused_client);
    cJSON_AddBoolToObject(stats, "paused_diag", st.paused_diag);
    cJSON_AddBoolToObject(stats, "paused_bus", st.paused_bus);
    ap_guard_status_json(o);
    cJSON_AddNumberToObject(stats, "polls_ok", st.polls_ok);
    cJSON_AddNumberToObject(stats, "polls_failed", st.polls_failed);
    /* the J1939 rows (autopid_j1939.h): looks into the listener's store,
       and how many of them published a message not published before */
    cJSON_AddNumberToObject(stats, "passive_ok", st.passive_ok);
    cJSON_AddNumberToObject(stats, "passive_failed", st.passive_failed);

    uint32_t published = 0, requested = 0, refused = 0;

    ap_runner_j1939_stats(&published, NULL);
    ap_runner_j1939_tx_stats(&requested, &refused);
    cJSON_AddNumberToObject(stats, "passive_published", published);
    cJSON_AddNumberToObject(stats, "passive_requested", requested);
    cJSON_AddNumberToObject(stats, "passive_refused", refused);
    cJSON_AddBoolToObject(stats, "j1939_listening", ap_j1939_listening());
    cJSON_AddBoolToObject(stats, "j1939_active", j1939_active());
    cJSON_AddNumberToObject(stats, "pids", st.pids_loaded);
    cJSON_AddNumberToObject(stats, "filters", st.filters_loaded);
    /* Phase 1b: ~53 ms/request measured floor; sub-floor periods are
       accepted but flagged so a UI can warn */
    cJSON_AddNumberToObject(stats, "period_floor_ms", AP_PERIOD_FLOOR_MS);
    cJSON_AddNumberToObject(stats, "sub_floor_pids",
                            ap_core_sub_floor_count());
    cJSON_AddNumberToObject(stats, "now_us",
                            (double)esp_timer_get_time());

    return ap_http_send_json(req, o);
}

static esp_err_t autopid_data_handler(httpd_req_t *req)
{
    cJSON *snap = NULL;

    if (autopid_snapshot(&snap) != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    return ap_http_send_json(req, snap);
}

esp_err_t ap_http_send_file(httpd_req_t *req, const char *path,
                                      const char *dflt);

static esp_err_t config_get_handler(httpd_req_t *req)
{
    return ap_http_send_file(req, autopid_config_path(),
                                "{\"groups\":[],\"pids\":[],"
                                "\"filters\":[]}");
}

static esp_err_t config_put_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > AP_HTTP_BODY_MAX)
    {
        return ap_http_send_error(req, "400 Bad Request", "missing/oversized body");
    }

    char *body = heap_caps_malloc(len + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (body == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            free(body);
            return ap_http_send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    /* validate BEFORE persisting (expressions, groups, bounds) */
    static ap_config_t s_probe EXT_RAM_BSS_ATTR; /* too big for a stack */
    char perr[96] = "";

    if (ap_config_parse(body, &s_probe, perr, sizeof(perr)) != ESP_OK)
    {
        free(body);
        return ap_http_send_error(req, "400 Bad Request", perr);
    }

    esp_err_t err = autopid_config_save(body, len);

    free(body);

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "save failed");
    }

    /* the config file applies LIVE (httpd task = internal stack: the
       reload's file read is §2-safe here) */
    err = autopid_reload_config();

    if (err != ESP_OK)
    {
        return ap_http_send_error(req, "500 Internal Server Error",
                          "reload failed");
    }

    ESP_LOGI(TAG, "config updated + reloaded (%u pids, %u params)",
             s_probe.n_pids, s_probe.n_params);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "pids", s_probe.n_pids);
    cJSON_AddNumberToObject(o, "params", s_probe.n_params);
    return ap_http_send_json(req, o);
}

esp_err_t ap_http_send_file(httpd_req_t *req, const char *path,
                                      const char *dflt) /* fwd-declared */
{
    size_t size = 0;

    if (filesystem_size(path, &size) != ESP_OK || size == 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, dflt);
    }

    char *buf = heap_caps_malloc(size + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    size_t got = 0;

    if (filesystem_read(path, buf, size, &got) != ESP_OK)
    {
        free(buf);
        return ap_http_send_error(req, "500 Internal Server Error", "read failed");
    }

    buf[got] = '\0';
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_sendstr(req, buf);

    free(buf);
    return err;
}

static esp_err_t std_scan_post_handler(httpd_req_t *req)
{
    esp_err_t err = autopid_std_scan_start();

    if (err == ESP_ERR_INVALID_STATE)
    {
        return ap_http_send_error(req, "409 Conflict", "scan already running");
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

static esp_err_t std_scan_get_handler(httpd_req_t *req)
{
    cJSON *o = ap_std_scan_status_json();

    if (o == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    return ap_http_send_json(req, o);
}

static esp_err_t std_scan_result_handler(httpd_req_t *req)
{
    return ap_http_send_file(req, autopid_std_scan_path(),
                                "{\"supported\":[],\"found\":0}");
}

static esp_err_t std_table_handler(httpd_req_t *req)
{
    cJSON *arr = ap_std_table_json();

    if (arr == NULL)
    {
        return ap_http_send_error(req, "500 Internal Server Error", "oom");
    }

    return ap_http_send_json(req, arr);
}

static esp_err_t group_post_handler(httpd_req_t *req)
{
    size_t len = req->content_len;

    if (len == 0 || len > 256)
    {
        return ap_http_send_error(req, "400 Bad Request", "missing/oversized body");
    }

    char body[257];
    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            return ap_http_send_error(req, "400 Bad Request", "body read failed");
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);

    if (root == NULL)
    {
        return ap_http_send_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *period = cJSON_GetObjectItem(root, "period_ms");

    if (!cJSON_IsString(name) || !cJSON_IsBool(enabled))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                          "need string 'name' + bool 'enabled'");
    }

    int32_t override_ms = -1; /* -1 = leave the period alone */

    if (cJSON_IsNumber(period) && period->valuedouble >= 0)
    {
        override_ms = (int32_t)period->valuedouble;
    }

    esp_err_t err = autopid_group_set(name->valuestring,
                                      cJSON_IsTrue(enabled), override_ms);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND)
    {
        return ap_http_send_error(req, "404 Not Found", "no such group");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    return ap_http_send_json(req, o);
}

esp_err_t autopid_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/autopid", .method = HTTP_GET,
          .handler = autopid_get_handler },
        { .uri = "/api/autopid/data", .method = HTTP_GET,
          .handler = autopid_data_handler },
        { .uri = "/api/autopid/config", .method = HTTP_GET,
          .handler = config_get_handler },
        { .uri = "/api/autopid/config", .method = HTTP_PUT,
          .handler = config_put_handler },
        { .uri = "/api/autopid/std_scan", .method = HTTP_POST,
          .handler = std_scan_post_handler },
        { .uri = "/api/autopid/std_scan", .method = HTTP_GET,
          .handler = std_scan_get_handler },
        { .uri = "/api/autopid/std_scan/result", .method = HTTP_GET,
          .handler = std_scan_result_handler },
        /* the vehicle store (autopid_http_vehicles.c): one wildcard per
           method, `detect` / `<key>` / `<key>/activate` dispatched inside */
        { .uri = "/api/autopid/vehicles", .method = HTTP_GET,
          .handler = vehicles_get_handler },
        { .uri = "/api/autopid/vehicles/*", .method = HTTP_POST,
          .handler = vehicles_post_handler },
        { .uri = "/api/autopid/vehicles/*", .method = HTTP_PUT,
          .handler = vehicles_put_handler },
        { .uri = "/api/autopid/vehicles/*", .method = HTTP_DELETE,
          .handler = vehicles_delete_handler },
        { .uri = "/api/autopid/std_table", .method = HTTP_GET,
          .handler = std_table_handler },
        { .uri = "/api/autopid/test", .method = HTTP_POST,
          .handler = test_post_handler },
        { .uri = "/api/autopid/group", .method = HTTP_POST,
          .handler = group_post_handler },
        { .uri = "/api/autopid/dtc", .method = HTTP_GET,
          .handler = dtc_get_handler },
        { .uri = "/api/autopid/dtc/scan", .method = HTTP_POST,
          .handler = dtc_scan_post_handler },
        { .uri = "/api/autopid/dtc/clear", .method = HTTP_POST,
          .handler = dtc_clear_post_handler },
        { .uri = "/api/autopid/dtc/db", .method = HTTP_GET,
          .handler = dtc_db_get_handler },
        { .uri = "/api/autopid/dtc/db", .method = HTTP_POST,
          .handler = dtc_db_post_handler },
        { .uri = "/api/autopid/dtc/db", .method = HTTP_DELETE,
          .handler = dtc_db_delete_handler },
        { .uri = "/api/autopid/dtc/db/search", .method = HTTP_GET,
          .handler = dtc_db_search_handler },
        { .uri = "/api/autopid/dtc/lookup", .method = HTTP_GET,
          .handler = dtc_lookup_handler },
        { .uri = "/api/autopid/dbc", .method = HTTP_GET,
          .handler = dbc_get_handler },
        { .uri = "/api/autopid/dbc", .method = HTTP_POST,
          .handler = dbc_post_handler },
        { .uri = "/api/autopid/dbc", .method = HTTP_DELETE,
          .handler = dbc_delete_handler },
        { .uri = "/api/autopid/dbc/signals", .method = HTTP_GET,
          .handler = dbc_signals_handler },
        { .uri = "/api/autopid/dbc/add", .method = HTTP_POST,
          .handler = dbc_add_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/autopid routes registered");
    }

    return err;
}
