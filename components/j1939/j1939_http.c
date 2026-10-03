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
 * @file j1939_http.c
 * @brief GET /api/j1939, three views of one route (§9.1 own-routes;
 *        HTTP_API.md):
 *
 *          /api/j1939               state, mode, the address claim and the
 *                                   transmit counters of active mode,
 *                                   counters, sources, the built-in values
 *                                   the bus carries, DM1 per controller
 *          /api/j1939?pgns=1        every stored message with its payload
 *          /api/j1939?pgn=FEEC      one group's newest message (optional
 *                                   &sa=, &da=; 404 when nobody sent it)
 *          /api/j1939?request=FECB  active mode: send a Request for that
 *                                   group (optional &da=, default everyone)
 *                                   and answer at once with the request's
 *                                   outcome so far; 409 in listen mode or
 *                                   without an address
 *
 * The reply is written piece by piece into a PSRAM block and sent whenever
 * the block fills up: no JSON tree, nothing large on the server's stack, and
 * the listener's lock is never held while a socket is written.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "can_manager.h"
#include "http_server_manager.h"

#include "j1939.h"
#include "j1939_private.h"

static const char *TAG = "j1939";

#define OUT_BLOCK   2048 /* reply block: pieces are far smaller            */
#define HEX_STEP    64   /* payload bytes per piece                        */
#define DM1_MAX     32   /* trouble codes listed per controller            */

typedef struct
{
    httpd_req_t *req;
    esp_err_t    err;
    char        *buf; /* OUT_BLOCK bytes, then J1939_MSG_MAX of payload room */
    size_t       len;
} out_t;

static void out_flush(out_t *o)
{
    if (o->err == ESP_OK && o->len > 0)
    {
        o->err = httpd_resp_send_chunk(o->req, o->buf, (ssize_t)o->len);
    }

    o->len = 0;
}

static void outf(out_t *o, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void outf(out_t *o, const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2 && o->err == ESP_OK; attempt++)
    {
        va_list ap;

        va_start(ap, fmt);

        int n = vsnprintf(o->buf + o->len, OUT_BLOCK - o->len, fmt, ap);

        va_end(ap);

        if (n < 0)
        {
            o->err = ESP_FAIL;
            return;
        }

        if ((size_t)n < OUT_BLOCK - o->len)
        {
            o->len += (size_t)n;
            return;
        }

        out_flush(o); /* did not fit: send what is there, then once more */
    }

    if (o->err == ESP_OK)
    {
        o->err = ESP_ERR_INVALID_SIZE; /* a piece larger than the block */
    }
}

static void out_hex(out_t *o, const uint8_t *data, size_t len)
{
    static const char HEX[] = "0123456789ABCDEF";

    for (size_t i = 0; i < len && o->err == ESP_OK; i += HEX_STEP)
    {
        char piece[HEX_STEP * 2 + 1];
        size_t n = (len - i < HEX_STEP) ? len - i : HEX_STEP;

        for (size_t k = 0; k < n; k++)
        {
            piece[2 * k] = HEX[data[i + k] >> 4];
            piece[2 * k + 1] = HEX[data[i + k] & 0x0Fu];
        }

        piece[2 * n] = '\0';
        outf(o, "%s", piece);
    }
}

static esp_err_t out_end(out_t *o)
{
    out_flush(o);

    if (o->err == ESP_OK)
    {
        o->err = httpd_resp_send_chunk(o->req, NULL, 0);
    }

    free(o->buf);
    o->buf = NULL;
    return o->err;
}

/** One stored message as an object. @p payload holds min(len, cap) bytes. */
static void out_message(out_t *o, const j1939_msg_t *m, const uint8_t *payload,
                        size_t cap)
{
    outf(o, "{\"pgn\":\"%04lX\",\"sa\":%u,\"da\":%u,\"len\":%u,\"count\":%lu,"
            "\"period_ms\":%lu,\"age_ms\":%lu,\"data\":\"",
         (unsigned long)m->pgn, m->sa, m->da, (unsigned)m->len,
         (unsigned long)m->count, (unsigned long)m->period_ms,
         (unsigned long)m->age_ms);
    out_hex(o, payload, (m->len < cap) ? m->len : cap);
    outf(o, "\"}");
}

/* ---- the three views ---------------------------------------------------------------- */

static void view_one(out_t *o, uint32_t pgn, int sa, int da)
{
    j1939_msg_t m;
    uint8_t *payload = (uint8_t *)o->buf + OUT_BLOCK;

    if (j1939_pgn_latest(pgn, sa, da, &m, payload, J1939_MSG_MAX) != ESP_OK)
    {
        httpd_resp_set_status(o->req, "404 Not Found");
        outf(o, "{\"error\":\"nobody sent this group\"}");
        return;
    }

    out_message(o, &m, payload, J1939_MSG_MAX);
}

static void view_list(out_t *o)
{
    j1939_msg_t m;
    uint8_t *payload = (uint8_t *)o->buf + OUT_BLOCK;
    size_t cursor = 0;
    bool first = true;

    outf(o, "{\"seq\":%lu,\"pgns\":[", (unsigned long)j1939_sequence());

    while (o->err == ESP_OK &&
           j1939_msg_next(&cursor, &m, payload, J1939_MSG_MAX))
    {
        outf(o, "%s", first ? "" : ",");
        first = false;
        out_message(o, &m, payload, J1939_MSG_MAX);
    }

    outf(o, "]}");
}

static void status_head(out_t *o)
{
    j1939_status_t st;
    can_manager_status_t can;
    char vin[J1939_VIN_LEN + 1];
    uint8_t vin_sa = 0;

    (void)j1939_status(&st);
    (void)can_manager_status(&can);

    outf(o, "{\"enabled\":%s,\"state\":\"%s\",\"bus\":\"%s\","
            "\"mode\":\"%s\",",
         j1939_settings_enabled() ? "true" : "false",
         j1939_state_name(st.state), j1939_bus_name(st.bus),
         st.active ? "active" : "listen");

    /* active mode: the address claim and what the node sent / answered */
    outf(o, "\"claim\":{\"state\":\"%s\",\"address\":%u,\"preferred\":%u,"
            "\"tx_ready\":%s,\"name\":\"",
         j1939_claim_state_name(st.claim), st.address,
         j1939_settings_address(), st.tx_ready ? "true" : "false");
    out_hex(o, st.name, sizeof(st.name));
    outf(o, "\",\"claims_sent\":%lu,\"contests\":%lu,\"won\":%lu,\"lost\":%lu,"
            "\"held\":%lu,\"requests_answered\":%lu,\"cannot\":%lu},",
         (unsigned long)st.claim_stats.claims_sent,
         (unsigned long)st.claim_stats.contests,
         (unsigned long)st.claim_stats.won, (unsigned long)st.claim_stats.lost,
         (unsigned long)st.claim_stats.held,
         (unsigned long)st.claim_stats.requests,
         (unsigned long)st.claim_stats.cannot);
    outf(o, "\"tx\":{\"frames\":%lu,\"failed\":%lu,\"requests\":%lu,"
            "\"acks\":%lu,\"nacks\":%lu,\"nacks_sent\":%lu,\"tp_to_me\":%lu,"
            "\"tp_cts\":%lu,\"tp_eoma\":%lu,\"tp_aborts\":%lu,"
            "\"tp_reply_lost\":%lu},",
         (unsigned long)st.tx_frames, (unsigned long)st.tx_failed,
         (unsigned long)st.requests, (unsigned long)st.acks,
         (unsigned long)st.nacks, (unsigned long)st.nacks_sent,
         (unsigned long)st.tp_to_me, (unsigned long)st.tp_cts,
         (unsigned long)st.tp_eoma, (unsigned long)st.tp_aborts_out,
         (unsigned long)st.tp_reply_lost);

    if (j1939_vin(vin, &vin_sa))
    {
        outf(o, "\"vin\":\"%s\",\"vin_sa\":%u,", vin, vin_sa);
    }
    else
    {
        outf(o, "\"vin\":null,");
    }

    outf(o, "\"can\":{\"running\":%s,\"baud_kbps\":%lu,\"link\":\"%s\","
            "\"listen_only\":%s},",
         can.running ? "true" : "false", (unsigned long)can.baud_kbps,
         can.link.state, can.link.listen_only ? "true" : "false");
    outf(o, "\"stats\":{\"rx_frames\":%lu,\"rx_data\":%lu,\"rx_tp_cm\":%lu,"
            "\"rx_tp_dt\":%lu,\"rx_diag\":%lu,\"rx_foreign\":%lu,"
            "\"queue_drops\":%lu,\"messages\":%lu,\"not_kept\":%lu,"
            "\"evicted\":%lu,\"long_evicted\":%lu,\"entries\":%u,"
            "\"entries_max\":%u,\"seq\":%lu},",
         (unsigned long)st.rx_frames, (unsigned long)st.rx_data,
         (unsigned long)st.rx_tp_cm, (unsigned long)st.rx_tp_dt,
         (unsigned long)st.rx_diag, (unsigned long)st.rx_foreign,
         (unsigned long)st.queue_drops, (unsigned long)st.messages,
         (unsigned long)st.not_kept, (unsigned long)st.evicted,
         (unsigned long)st.long_evicted, (unsigned)st.entries,
         (unsigned)st.entries_cap, (unsigned long)j1939_sequence());
    outf(o, "\"tp\":{\"open\":%u,\"max\":%u,\"started\":%lu,"
            "\"completed\":%lu,\"seq_errors\":%lu,\"timeouts\":%lu,"
            "\"aborted\":%lu,\"replaced\":%lu,\"no_session\":%lu,"
            "\"orphan_dt\":%lu,\"bad_cm\":%lu},",
         (unsigned)st.tp_open, (unsigned)st.tp_cap,
         (unsigned long)st.tp_started, (unsigned long)st.tp_completed,
         (unsigned long)st.tp_seq_errors, (unsigned long)st.tp_timeouts,
         (unsigned long)st.tp_aborted, (unsigned long)st.tp_replaced,
         (unsigned long)st.tp_no_session, (unsigned long)st.tp_orphan_dt,
         (unsigned long)st.tp_bad_cm);
}

static void status_values(out_t *o)
{
    size_t count = 0;
    const j1939_spn_t *table = j1939_spn_table(&count);
    bool first = true;

    outf(o, "\"values\":[");

    for (size_t i = 0; i < count; i++)
    {
        j1939_msg_t m;
        j1939_raw_t raw = J1939_RAW_SHORT;
        double value = 0;

        if (j1939_spn_latest(&table[i], J1939_ADDR_ANY, &value, &raw, &m) !=
            ESP_OK)
        {
            continue; /* nobody sends its group */
        }

        outf(o, "%s{\"name\":\"%s\",\"spn\":%lu,\"pgn\":\"%04lX\",\"sa\":%u,"
                "\"state\":\"%s\",",
             first ? "" : ",", table[i].name, (unsigned long)table[i].spn,
             (unsigned long)table[i].pgn, m.sa, j1939_raw_name(raw));
        first = false;

        if (raw == J1939_RAW_VALID)
        {
            outf(o, "\"value\":%.10g,", value);
        }

        outf(o, "\"unit\":\"%s\",\"age_ms\":%lu,\"period_ms\":%lu}",
             table[i].unit, (unsigned long)m.age_ms,
             (unsigned long)m.period_ms);
    }

    outf(o, "],");
}

static void status_sources_and_dm1(out_t *o)
{
    bool first = true;

    outf(o, "\"sources\":[");

    for (unsigned sa = 0; sa < 256; sa++)
    {
        j1939_source_t s;

        if (!j1939_source((uint8_t)sa, &s))
        {
            continue;
        }

        outf(o, "%s{\"sa\":%u,\"frames\":%lu,\"age_ms\":%lu,\"name\":",
             first ? "" : ",", sa, (unsigned long)s.frames,
             (unsigned long)s.age_ms);
        first = false;

        if (s.named)
        {
            outf(o, "\"");
            out_hex(o, s.name, sizeof(s.name));
            outf(o, "\"}");
        }
        else
        {
            outf(o, "null}");
        }
    }

    outf(o, "],\"dm1\":[");
    first = true;

    for (unsigned sa = 0; sa < 256; sa++)
    {
        j1939_lamps_t lamps;
        j1939_dtc_t dtc[DM1_MAX];
        j1939_msg_t m;
        size_t codes = 0;

        if (j1939_dm1((uint8_t)sa, &lamps, dtc, DM1_MAX, &codes, &m) != ESP_OK)
        {
            continue;
        }

        outf(o, "%s{\"sa\":%u,\"age_ms\":%lu,\"mil\":%u,\"rsl\":%u,"
                "\"awl\":%u,\"pl\":%u,\"count\":%u,\"dtcs\":[",
             first ? "" : ",", sa, (unsigned long)m.age_ms, lamps.mil,
             lamps.rsl, lamps.awl, lamps.pl, (unsigned)codes);
        first = false;

        for (size_t k = 0; k < codes && k < DM1_MAX; k++)
        {
            char text[J1939_DTC_TEXT_LEN];

            j1939_dtc_text(dtc[k].spn, dtc[k].fmi, text, sizeof(text));
            outf(o, "%s{\"code\":\"%s\",\"spn\":%lu,\"fmi\":%u,\"oc\":%u,"
                    "\"cm\":%s}",
                 (k == 0) ? "" : ",", text, (unsigned long)dtc[k].spn,
                 dtc[k].fmi, dtc[k].oc, dtc[k].cm ? "true" : "false");
        }

        outf(o, "]}");
    }

    outf(o, "]}");
}

/** ?request=<pgn>[&da=]: ask the network (active mode) and report. */
static void view_request(out_t *o, uint32_t pgn, int da)
{
    uint8_t dest = (da >= 0) ? (uint8_t)da : J1939_ADDR_GLOBAL;
    esp_err_t err = j1939_request(pgn, dest);

    if (err == ESP_ERR_INVALID_STATE)
    {
        httpd_resp_set_status(o->req, "409 Conflict");
        outf(o, "{\"error\":\"%s\"}",
             j1939_settings_active()
                 ? "no address on the bus yet (claim pending, lost, or the "
                   "bus is listen-only)"
                 : "listen mode: the node never transmits (j1939.mode)");
        return;
    }

    if (err != ESP_OK)
    {
        httpd_resp_set_status(o->req, "503 Service Unavailable");
        outf(o, "{\"error\":\"the bus did not take the frame\"}");
        return;
    }

    outf(o, "{\"sent\":true,\"pgn\":\"%04lX\",\"da\":%u,\"from\":%u,"
            "\"outcome\":\"%s\"}", (unsigned long)pgn, dest,
         j1939_address(),
         j1939_req_outcome_name(j1939_request_outcome(pgn, dest, NULL, NULL)));
}

/* ---- the route ------------------------------------------------------------------------ */

static esp_err_t get_handler(httpd_req_t *req)
{
    char query[64];
    char val[16];
    bool list = false;
    long pgn = -1;
    long request = -1;
    int sa = J1939_ADDR_ANY;
    int da = J1939_ADDR_ANY;
    out_t o = { .req = req, .err = ESP_OK };

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "pgns", val, sizeof(val)) == ESP_OK)
        {
            list = (val[0] == '1');
        }

        if (httpd_query_key_value(query, "request", val, sizeof(val)) == ESP_OK)
        {
            request = strtol(val, NULL, 16);
        }

        if (httpd_query_key_value(query, "pgn", val, sizeof(val)) == ESP_OK)
        {
            pgn = strtol(val, NULL, 16);
        }

        if (httpd_query_key_value(query, "sa", val, sizeof(val)) == ESP_OK)
        {
            sa = (int)(strtol(val, NULL, 0) & 0xFF);
        }

        if (httpd_query_key_value(query, "da", val, sizeof(val)) == ESP_OK)
        {
            da = (int)(strtol(val, NULL, 0) & 0xFF);
        }
    }

    /* one block: the reply pieces, then room for the largest message */
    o.buf = heap_caps_malloc(OUT_BLOCK + J1939_MSG_MAX, MALLOC_CAP_SPIRAM);

    if (o.buf == NULL)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
    }

    httpd_resp_set_type(req, "application/json");

    if (request >= 0)
    {
        view_request(&o, (uint32_t)request & 0x3FFFFu, da);
    }
    else if (pgn >= 0)
    {
        view_one(&o, (uint32_t)pgn & 0x3FFFFu, sa, da);
    }
    else if (list)
    {
        view_list(&o);
    }
    else
    {
        status_head(&o);
        status_values(&o);
        status_sources_and_dm1(&o);
    }

    return out_end(&o);
}

esp_err_t j1939_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/j1939", .method = HTTP_GET, .handler = get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/j1939 registered");
    }

    return err;
}
