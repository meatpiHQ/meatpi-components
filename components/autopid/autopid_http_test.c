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
 * @file autopid_http_test.c
 * @brief POST /api/autopid/test (test-a-PID, §11): one shot through the
 *        real runner path for a chip request (type init, PID init, ATCRA,
 *        request, the transcript of the exchange), or, for a J1939 group
 *        (`PGN:<hex>[@<source>][?]`, 2026-10-03), the newest message of
 *        that group in the listener's store, nothing sent, or, for a CAN
 *        filter (`"filter"`, 2026-11-14), one monitor window for its
 *        frame. Split out of
 *        autopid_http.c 2026-10-03 (700-line rule). The route table stays
 *        in autopid_http.c.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_http_server.h"

#include "battery_monitor.h"
#include "expression_parser.h"
#include "j1939.h"

#include "autopid.h"
#include "autopid_bus.h"
#include "autopid_private.h"
#include "autopid_http_private.h"

/** The bytes of @p data as hex with spaces ("02 1E 0B"), cut at @p cap. */
static void hex_line(const uint8_t *data, size_t n, char *out, size_t cap)
{
    size_t w = 0;

    out[0] = '\0';

    for (size_t i = 0; i < n && w + 4 < cap; i++)
    {
        w += (size_t)snprintf(out + w, cap - w, "%02X%s", data[i],
                              (i + 1 < n) ? " " : "");
    }
}

/* test-a-filter (2026-11-14): "filter" is a CAN filter as the tables hold
   it (frame_id, monitor_ms, parameters with their expression and, for a
   multiplexed signal, mux_expr / mux_val). ONE monitor window, what a poll
   of the filter opens: nothing is asked on the bus, nothing is published.
   Every parameter is decoded from the frame it would be in a poll (the
   first one, or the one of its page). */
static esp_err_t test_filter(httpd_req_t *req, const cJSON *flt, char *tr,
                             size_t tr_len)
{
    static ap_flt_want_t s_want[AP_PARAMS_PER] EXT_RAM_BSS_ATTR; /* httpd */
    static char s_hex[AP_PAYLOAD_MAX * 3 + 4] EXT_RAM_BSS_ATTR;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(flt, "frame_id");
    const cJSON *mon = cJSON_GetObjectItemCaseSensitive(flt, "monitor_ms");
    const cJSON *prms = cJSON_GetObjectItemCaseSensitive(flt, "parameters");
    double monitor_ms = cJSON_IsNumber(mon) ? mon->valuedouble : 1000;

    if (!cJSON_IsNumber(id) || id->valuedouble < 1 ||
        id->valuedouble > 0x1FFFFFFF)
    {
        return ap_http_send_error(req, "400 Bad Request",
                                  "filter: frame_id 1 to 0x1FFFFFFF required");
    }

    if (monitor_ms < 50 || monitor_ms > 60000)
    {
        return ap_http_send_error(req, "400 Bad Request",
                                  "filter: monitor_ms 50..60000");
    }

    if (cJSON_GetArraySize(prms) > AP_PARAMS_PER)
    {
        return ap_http_send_error(req, "400 Bad Request",
                                  "filter: too many parameters");
    }

    const cJSON *p = NULL;
    uint16_t n = 0;
    char eerr[64];

    cJSON_ArrayForEach(p, prms)
    {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(p, "expression");
        const cJSON *mx = cJSON_GetObjectItemCaseSensitive(p, "mux_expr");
        const cJSON *mv = cJSON_GetObjectItemCaseSensitive(p, "mux_val");
        bool muxed = cJSON_IsString(mx) && mx->valuestring[0] != '\0';

        if (!cJSON_IsString(e) || e->valuestring[0] == '\0' ||
            expression_parser_check(e->valuestring, NULL, eerr,
                                    sizeof(eerr)) != ESP_OK ||
            (muxed && expression_parser_check(mx->valuestring, NULL, eerr,
                                              sizeof(eerr)) != ESP_OK))
        {
            return ap_http_send_error(req, "400 Bad Request",
                                      "filter: bad expression in "
                                      "parameters[]");
        }

        s_want[n].mux_expr = muxed ? mx->valuestring : NULL;
        s_want[n].mux_val = cJSON_IsNumber(mv) ? (float)mv->valuedouble : 0;
        s_want[n].want = true;
        n++;
    }

    uint16_t slots = n;

    if (slots == 0)
    {
        /* no parameter yet: the frame itself is the answer */
        s_want[0] = (ap_flt_want_t){ NULL, 0, true };
        slots = 1;
    }

    char guard_reason[176];

    if (!ap_guard_job_ok(guard_reason, sizeof(guard_reason)))
    {
        return ap_http_send_error(req, "409 Conflict", guard_reason);
    }

    if (!ap_core_job_acquire()) /* vs std scan / dtc jobs / other tests */
    {
        return ap_http_send_error(req, "409 Conflict",
                                  "another chip job runs");
    }

    int64_t elapsed_us = 0;

    ap_core_scan_pause(true);   /* park the poller around the window */

    esp_err_t err = ap_runner_test_filter((uint32_t)id->valuedouble,
                                          (uint32_t)monitor_ms, s_want, slots,
                                          &elapsed_us, tr, tr_len);

    /* the poller stays parked until the frames are read out: its next
       window takes the same slots (bench 2026-11-14: the second page of a
       multiplexed message was gone by the time it was decoded) */
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(o, "elapsed_ms", (double)elapsed_us / 1000.0);

    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(o, "error",
                                (err == ESP_ERR_TIMEOUT)
                                    ? "no frame with this CAN ID in the "
                                      "listen time"
                                : (err == ESP_ERR_INVALID_STATE)
                                    ? "chip busy (monitor/update)"
                                    : "the chip did not open the window");
    }

    cJSON_AddStringToObject(o, "transcript", tr);

    /* the data bytes the expressions index (B0 = the first data byte): the
       first frame the window kept */
    for (uint16_t i = 0; err == ESP_OK && i < slots; i++)
    {
        size_t len = 0;
        const uint8_t *d = ap_flt_slot(i, &len);

        if (d != NULL)
        {
            hex_line(d, len, s_hex, sizeof(s_hex));
            cJSON_AddStringToObject(o, "payload", s_hex);
            break;
        }
    }

    /* one entry per parameter, null where its frame did not come */
    cJSON *vals = cJSON_AddArrayToObject(o, "values");
    float volts = 0;
    uint16_t i = 0;

    (void)battery_monitor_voltage(&volts);

    cJSON_ArrayForEach(p, prms)
    {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(p, "expression");
        size_t len = 0;
        const uint8_t *d = (err == ESP_OK) ? ap_flt_slot(i, &len) : NULL;
        double value = 0;

        if (d != NULL && expression_parser_eval(e->valuestring, d, len,
                                                (double)volts,
                                                &value) == ESP_OK)
        {
            cJSON_AddItemToArray(vals, cJSON_CreateNumber(value));
        }
        else
        {
            cJSON_AddItemToArray(vals, cJSON_CreateNull());
        }

        i++;
    }

    ap_core_scan_pause(false);
    ap_core_job_release();
    return ap_http_send_json(req, o);
}

/* test-a-PID (§11): one-shot through the REAL runner path, the ws
   console can't reproduce init/rxheader/expression handling */
esp_err_t test_post_handler(httpd_req_t *req)
{
    /* PSRAM buffers: an "expressions" list for a 96-parameter DID (the
       bZ4X and Leaf cell voltages, 2026-10-24: the 2 KB cap cut the
       bZ4X's list and the parse failed as "cmd required") does not fit
       a 512 B stack body, and the transcript is ~1 KB. Handlers run on
       the one httpd task, so plain statics are safe; the chip itself is
       guarded by the job lock below. */
    static char s_body[6144] EXT_RAM_BSS_ATTR;
    static char s_raw[AP_RESP_MAX] EXT_RAM_BSS_ATTR;
    static char s_tr[1536] EXT_RAM_BSS_ATTR;

    int len = httpd_req_recv(req, s_body, sizeof(s_body) - 1);

    if (len <= 0)
    {
        return ap_http_send_error(req, "400 Bad Request", "missing body");
    }

    s_body[len] = '\0';

    cJSON *root = cJSON_Parse(s_body);
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *init = cJSON_GetObjectItemCaseSensitive(root, "init");
    const cJSON *rxh = cJSON_GetObjectItemCaseSensitive(root,
                                                        "rxheader");
    const cJSON *expr = cJSON_GetObjectItemCaseSensitive(root,
                                                         "expression");
    const cJSON *exprs = cJSON_GetObjectItemCaseSensitive(root,
                                                          "expressions");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *bus_j = cJSON_GetObjectItemCaseSensitive(root, "bus");
    const cJSON *kbps_j = cJSON_GetObjectItemCaseSensitive(root, "bus_kbps");
    ap_row_bus_t bus = AP_BUS_OBD;
    uint8_t bus_rate = 0;

    /* "filter": a CAN filter instead of a request (test_filter above) */
    const cJSON *flt = cJSON_GetObjectItemCaseSensitive(root, "filter");

    if (cJSON_IsObject(flt))
    {
        esp_err_t sent = test_filter(req, flt, s_tr, sizeof(s_tr));

        cJSON_Delete(root);
        return sent;
    }

    /* "bus" and "bus_kbps": the row's bus and its speed (autopid_bus.h);
       the shot is then a poll of a row there: no type init, and the chip
       is home again behind it */
    if (bus_j != NULL &&
        (!cJSON_IsString(bus_j) || !ap_bus_parse(bus_j->valuestring, &bus)))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                                  "unknown bus (obd, ms, sw or ch)");
    }

    if (!ap_bus_rate_parse((uint8_t)bus,
                           cJSON_IsNumber(kbps_j) ? kbps_j->valuedouble : 0,
                           &bus_rate))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                                  "bus_kbps needs a bus other than obd and a "
                                  "speed the chip can make (500 over 1 to "
                                  "64, or that times 8/7)");
    }

    if (!cJSON_IsString(cmd) || cmd->valuestring[0] == '\0' ||
        strlen(cmd->valuestring) >= AP_CMD_LEN ||
        (cJSON_IsString(init) &&
         strlen(init->valuestring) >= AP_INIT_LEN) ||
        (cJSON_IsString(rxh) && strlen(rxh->valuestring) >= AP_HDR_LEN))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                          "cmd required (init/rxheader length caps)");
    }

    char eerr[64];

    if (cJSON_IsString(expr) && expr->valuestring[0] != '\0' &&
        expression_parser_check(expr->valuestring, NULL, eerr,
                                sizeof(eerr)) != ESP_OK)
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request", "bad expression");
    }

    /* "expressions": decode the ONE reply with every parameter of the
       PID (the UI used to fire one request per parameter) */
    if (cJSON_IsArray(exprs))
    {
        if (cJSON_GetArraySize(exprs) > AP_PARAMS_PER)
        {
            cJSON_Delete(root);
            return ap_http_send_error(req, "400 Bad Request",
                              "too many expressions");
        }

        const cJSON *e = NULL;

        cJSON_ArrayForEach(e, exprs)
        {
            if (!cJSON_IsString(e) || e->valuestring[0] == '\0' ||
                expression_parser_check(e->valuestring, NULL, eerr,
                                        sizeof(eerr)) != ESP_OK)
            {
                cJSON_Delete(root);
                return ap_http_send_error(req, "400 Bad Request",
                                  "bad expression in expressions[]");
            }
        }
    }

    /* "type": prepend the type init chain the poller sends when it
       switches to this PID's type, the shot then IS a poll of that PID */
    int tidx = -1;

    if (cJSON_IsString(type))
    {
        const char *t = type->valuestring;

        tidx = (strcmp(t, "std") == 0)      ? AP_PID_STD
             : (strcmp(t, "custom") == 0)   ? AP_PID_CUSTOM
             : (strcmp(t, "specific") == 0) ? AP_PID_SPECIFIC
                                            : -1;
    }

    /* a J1939 group (PGN:<hex>[@<source>][?]): decoded over the newest
       message in the listener's store, nothing sent, no chip job */
    uint32_t pgn = 0;
    int16_t pgn_sa = J1939_ADDR_ANY;
    bool pgn_req = false;
    bool is_pgn = (ap_pgn_cmd_parse(cmd->valuestring, &pgn, &pgn_sa,
                                    &pgn_req) == ESP_OK);
    static uint8_t s_payload[J1939_MSG_MAX] EXT_RAM_BSS_ATTR;
    uint8_t *payload = s_payload;
    size_t n = 0;
    int64_t elapsed_us = 0;
    esp_err_t err;

    /* a word the chip stores (a protocol preset, a link word): a test would
       be one write of its memory per click, on any bus */
    char stored[16];

    if (ap_chain_store_word(cmd->valuestring, stored, sizeof(stored)) ||
        (cJSON_IsString(init) &&
         ap_chain_store_word(init->valuestring, stored, sizeof(stored))))
    {
        char msg[112];

        snprintf(msg, sizeof(msg), "%s is stored by the OBD chip every time "
                 "it is sent; choose the row's Bus instead", stored);
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request", msg);
    }

    /* the type's own init holds such a word: the poller does not ask the
       rows of that type on the car's protocol (the header that follows the
       word was meant for another bus), and a test of one is no different */
    if (!is_pgn && bus == AP_BUS_OBD && ap_runner_type_held(tidx)[0] != '\0')
    {
        char msg[192];

        snprintf(msg, sizeof(msg), "%s holds %s, which the OBD chip stores "
                 "every time it is sent: PIDs of this type on the car's OBD "
                 "protocol are not asked. Take it out of the init, or choose "
                 "the PID's Bus", ap_runner_type_init_name(tidx),
                 ap_runner_type_held(tidx));
        cJSON_Delete(root);
        return ap_http_send_error(req, "409 Conflict", msg);
    }

    if (bus != AP_BUS_OBD &&
        (is_pgn || !ap_bus_chain_allowed(cmd->valuestring) ||
         (cJSON_IsString(init) && !ap_bus_chain_allowed(init->valuestring))))
    {
        cJSON_Delete(root);
        return ap_http_send_error(req, "400 Bad Request",
                                  "a row on another bus cannot be a PGN row, "
                                  "and its init and cmd must not pick a "
                                  "protocol or reset the chip");
    }

    if (is_pgn)
    {
        err = ap_runner_test_j1939(pgn, pgn_sa, payload, sizeof(s_payload),
                                   &n, s_tr, sizeof(s_tr));
        s_raw[0] = '\0';
    }
    else
    {
        char guard_reason[176];

        /* the bus guard listens on the HS pins: it has no say about a
           request on another bus's */
        if (bus == AP_BUS_OBD &&
            !ap_guard_job_ok(guard_reason, sizeof(guard_reason)))
        {
            cJSON_Delete(root);
            return ap_http_send_error(req, "409 Conflict", guard_reason);
        }

        if (!ap_core_job_acquire()) /* vs std scan / dtc jobs / other tests */
        {
            cJSON_Delete(root);
            return ap_http_send_error(req, "409 Conflict",
                                      "another chip job runs");
        }

        ap_core_scan_pause(true);   /* park the poller around the one-shot */

        err = ap_runner_test(
            tidx, AP_BUS_KEY(bus, bus_rate),
            (tidx >= 0 && bus == AP_BUS_OBD) ? ap_runner_type_init(tidx)
                                             : NULL,
            cJSON_IsString(init) ? init->valuestring : NULL,
            cJSON_IsString(rxh) ? rxh->valuestring : NULL, cmd->valuestring,
            s_raw, sizeof(s_raw), &elapsed_us, s_tr, sizeof(s_tr));

        ap_core_scan_pause(false);

        if (err == ESP_ERR_NOT_ALLOWED)
        {
            /* a chain of the row sets a protocol at another bit rate than
               the bus runs at: nothing went out, the guard says why */
            ap_core_job_release();
            cJSON_Delete(root);
            return ap_http_send_guard_error(req);
        }
    }

    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(o, "elapsed_ms",
                            (double)elapsed_us / 1000.0);

    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(o, "error",
                                is_pgn ? "the group is not in the listener's "
                                         "store (nobody sent it, or the "
                                         "listener is off)"
                                : (err == ESP_ERR_INVALID_STATE)
                                    ? "chip busy (monitor/update)"
                                    : "request failed/timeout");
        s_raw[0] = '\0';
    }

    cJSON_AddStringToObject(o, "raw", s_raw);
    cJSON_AddStringToObject(o, "transcript", s_tr);

    /* the bytes the expressions index: a chip reply as the chip printed it
       with headers on (ap_resp_frames: the legacy frame of reference, the
       published profiles'; 2026-10-24, issue #1014), a J1939 group's
       message as the listener holds it. PSRAM statics: httpd task only. */
    static uint8_t s_frames[AP_FRAMES_MAX] EXT_RAM_BSS_ATTR;
    static char s_hex[AP_FRAMES_MAX * 3 + 4] EXT_RAM_BSS_ATTR;
    bool have_payload = (err == ESP_OK);

    if (have_payload && !is_pgn)
    {
        if (bus != AP_BUS_OBD)
        {
            ap_resp_set_id29_hint(0, false); /* the slots are 11-bit */
            ap_resp_set_proto_hint('\0');    /* and CAN, whatever the car is */
        }
        else
        {
            ap_runner_reply_hint(
                (tidx >= 0) ? ap_runner_type_init(tidx) : NULL,
                cJSON_IsString(init) ? init->valuestring : NULL,
                cJSON_IsString(rxh) ? rxh->valuestring : NULL);
        }

        have_payload = (ap_resp_frames(s_raw, s_frames, sizeof(s_frames),
                                       &n) == ESP_OK);
        payload = s_frames;
    }

    if (have_payload)
    {
        /* the first 256 bytes as hex (a transport-protocol message can be
           longer: the expressions still see all of it) */
        size_t w = 0;
        size_t shown = (n < AP_FRAMES_MAX) ? n : AP_FRAMES_MAX;

        s_hex[0] = '\0';

        for (size_t i = 0; i < shown && w + 4 < sizeof(s_hex); i++)
        {
            w += (size_t)snprintf(s_hex + w, sizeof(s_hex) - w, "%02X%s",
                                  payload[i], (i + 1 < shown) ? " " : "");
        }

        if (shown < n && w + 4 < sizeof(s_hex))
        {
            snprintf(s_hex + w, sizeof(s_hex) - w, "...");
        }

        cJSON_AddStringToObject(o, "payload", s_hex);
    }
    else if (err == ESP_OK)
    {
        cJSON_AddStringToObject(o, "error", "no payload in response");
    }

    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    if (have_payload && cJSON_IsString(expr) && expr->valuestring[0] != '\0')
    {
        double value = 0;

        if (expression_parser_eval(expr->valuestring, payload, n,
                                   (double)volts, &value) == ESP_OK)
        {
            cJSON_AddNumberToObject(o, "value", value);
        }
        else
        {
            cJSON_AddStringToObject(o, "error",
                                    "expression failed on payload");
        }
    }

    if (cJSON_IsArray(exprs))
    {
        /* one entry per expression, null where the reply gave nothing */
        cJSON *vals = cJSON_AddArrayToObject(o, "values");
        const cJSON *e = NULL;

        cJSON_ArrayForEach(e, exprs)
        {
            double value = 0;

            if (have_payload &&
                expression_parser_eval(e->valuestring, payload, n,
                                       (double)volts, &value) == ESP_OK)
            {
                cJSON_AddItemToArray(vals, cJSON_CreateNumber(value));
            }
            else
            {
                cJSON_AddItemToArray(vals, cJSON_CreateNull());
            }
        }
    }

    if (!is_pgn)
    {
        ap_core_job_release();
    }

    cJSON_Delete(root);
    return ap_http_send_json(req, o);
}
