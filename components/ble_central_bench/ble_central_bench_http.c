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
 * @file ble_central_bench_http.c
 * @brief `/api/ble_bench`: GET = state, peer link parameters, discovered
 *        handles, the last result and counters; POST = connect /
 *        disconnect / run {mode, seconds, size}. The bench driver's whole
 *        surface (a PC script over USB-NCM or WiFi).
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "ble_central_bench_private.h"

static const char *TAG = BCB_TAG;

static esp_err_t send_json(httpd_req_t *req, cJSON *o, const char *status)
{
    char *s = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (s == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
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

static esp_err_t get_handler(httpd_req_t *req)
{
    bcb_status_t st;

    ble_central_bench_get_status(&st);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "enabled", ble_central_bench_is_enabled());
    cJSON_AddStringToObject(o, "state", ble_central_bench_state_name(st.state));

    cJSON *p = cJSON_AddObjectToObject(o, "peer");

    cJSON_AddStringToObject(p, "name", st.peer.name);
    cJSON_AddStringToObject(p, "addr", st.peer.addr);
    cJSON_AddNumberToObject(p, "mtu", st.peer.mtu);
    cJSON_AddNumberToObject(p, "itvl_ms", st.peer.itvl_units * 1.25);
    cJSON_AddNumberToObject(p, "phy_tx", st.peer.phy_tx);
    cJSON_AddNumberToObject(p, "phy_rx", st.peer.phy_rx);
    cJSON_AddNumberToObject(p, "dle_tx", st.peer.dle_tx);
    cJSON_AddNumberToObject(p, "dle_rx", st.peer.dle_rx);
    cJSON_AddBoolToObject(p, "secured", st.peer.secured);
    cJSON_AddBoolToObject(p, "bonded", st.peer.bonded);
    cJSON_AddNumberToObject(p, "rssi", st.peer.rssi);

    cJSON *c = cJSON_AddObjectToObject(o, "chars");

    cJSON_AddNumberToObject(c, "fff1", st.chars.fff1);
    cJSON_AddNumberToObject(c, "fff2", st.chars.fff2);
    cJSON_AddNumberToObject(c, "fff3", st.chars.fff3);
    cJSON_AddNumberToObject(c, "fff4", st.chars.fff4);
    cJSON_AddNumberToObject(c, "dis", st.chars.dis_mfr);

    cJSON *l = cJSON_AddObjectToObject(o, "last");

    cJSON_AddStringToObject(l, "mode", ble_central_bench_mode_name(st.last.mode));
    cJSON_AddBoolToObject(l, "running", st.last.running);
    cJSON_AddBoolToObject(l, "ok", st.last.ok);
    cJSON_AddNumberToObject(l, "bytes", st.last.bytes);
    cJSON_AddNumberToObject(l, "count", st.last.count);
    cJSON_AddNumberToObject(l, "ms", st.last.ms);
    cJSON_AddNumberToObject(l, "kbps", st.last.kbps);
    cJSON_AddNumberToObject(l, "errors", st.last.errors);
    cJSON_AddNumberToObject(l, "retries", st.last.retries);
    cJSON_AddNumberToObject(l, "http_status", st.last.http_status);
    cJSON_AddBoolToObject(l, "exact", st.last.exact);
    cJSON_AddNumberToObject(l, "holes", st.last.holes);
    cJSON_AddNumberToObject(l, "credits", st.last.credits);
    cJSON_AddStringToObject(l, "out", st.last.out == BCB_OUT_INDICATE ? "indicate"
                                      : st.last.out == BCB_OUT_NOTIFY ? "notify" : "");
    cJSON_AddStringToObject(l, "detail", st.last.detail);

    cJSON *k = cJSON_AddObjectToObject(o, "counters");

    cJSON_AddNumberToObject(k, "connects", st.counters.connects);
    cJSON_AddNumberToObject(k, "disconnects", st.counters.disconnects);
    cJSON_AddNumberToObject(k, "pair_ok", st.counters.pair_ok);
    cJSON_AddNumberToObject(k, "pair_fail", st.counters.pair_fail);
    cJSON_AddNumberToObject(k, "notify_rx", st.counters.notify_rx);
    cJSON_AddNumberToObject(k, "notify_bytes", st.counters.notify_bytes);
    cJSON_AddNumberToObject(k, "write_tx", st.counters.write_tx);
    cJSON_AddNumberToObject(k, "write_bytes", st.counters.write_bytes);
    cJSON_AddNumberToObject(k, "tunnel_frames_rx", st.counters.tunnel_frames_rx);
    cJSON_AddNumberToObject(k, "tunnel_frames_tx", st.counters.tunnel_frames_tx);
    cJSON_AddNumberToObject(k, "tunnel_resync", st.counters.tunnel_resync);
    cJSON_AddNumberToObject(k, "last_disconnect_reason", st.counters.last_disconnect_reason);

    {
        static char lines[BCB_DIAG_LINES][BCB_DIAG_LEN];
        int n = bcb_diag_lines(lines, BCB_DIAG_LINES);
        cJSON *lg = cJSON_AddArrayToObject(o, "log");

        for (int i = 0; i < n; i++)
        {
            cJSON_AddItemToArray(lg, cJSON_CreateString(lines[i]));
        }
    }

    return send_json(req, o, NULL);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    char body[256];
    int  n = httpd_req_recv(req, body, sizeof(body) - 1);

    if (n <= 0)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
    }

    body[n] = '\0';

    cJSON *in = cJSON_Parse(body);

    if (in == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }

    const cJSON *a = cJSON_GetObjectItemCaseSensitive(in, "action");
    const char *action = cJSON_IsString(a) ? a->valuestring : "";
    esp_err_t err;

    if (strcmp(action, "connect") == 0)
    {
        err = ble_central_bench_connect();
    }
    else if (strcmp(action, "disconnect") == 0)
    {
        err = ble_central_bench_disconnect();
    }
    else if (strcmp(action, "run") == 0)
    {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(in, "mode");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(in, "seconds");
        const cJSON *z = cJSON_GetObjectItemCaseSensitive(in, "size");
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(in, "out");
        bcb_mode_t mode = ble_central_bench_mode_parse(cJSON_IsString(m) ? m->valuestring : "");
        uint8_t out = (cJSON_IsString(o) && strcmp(o->valuestring, "indicate") == 0)
                          ? BCB_OUT_INDICATE : BCB_OUT_NOTIFY;

        if (mode == BCB_MODE_NONE)
        {
            cJSON_Delete(in);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode");
        }

        err = ble_central_bench_run(mode,
                                    cJSON_IsNumber(s) ? (uint32_t)s->valuedouble : 10,
                                    cJSON_IsNumber(z) ? (uint32_t)z->valuedouble : 65536,
                                    out);
    }
    else
    {
        cJSON_Delete(in);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action");
    }

    cJSON_Delete(in);

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "err", err == ESP_OK ? "" : esp_err_to_name(err));

    return send_json(req, o, err == ESP_OK ? NULL : "409 Conflict");
}

esp_err_t ble_central_bench_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/ble_bench", .method = HTTP_GET,  .handler = get_handler  },
        { .uri = "/api/ble_bench", .method = HTTP_POST, .handler = post_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/ble_bench registered");
    }

    return err;
}
