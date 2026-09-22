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
 * @file ble_http_core.c
 * @brief Pure tunnel core: frame reassembly with resync, REQ head parsing
 *        and validation (method, `/api/` prefix, printable path, body
 *        length guard), the request state machine (IDLE -> BODY ->
 *        forward -> stream the response through the pump), CREDIT flow
 *        control both ways, the body-frame counter (v2), for
 *        write-without-response uploads, ABORT / idle timeout / link-down
 *        handling. No FreeRTOS, no esp_http_client: the HTTP side is the
 *        injected bleh_ops_t, the BLE side the emit callback.
 */
#include "ble_http_core.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* ---- frame codec ---------------------------------------------------------------- */

bool bleh_hdr_parse(const uint8_t *b, bleh_hdr_t *h)
{
    if (b[0] != BLEH_MAGIC || b[1] != BLEH_VERSION)
    {
        return false;
    }

    if (b[2] < BLEH_T_REQ || b[2] > BLEH_T_CREDIT)
    {
        return false;
    }

    uint16_t len = (uint16_t)(b[6] | ((uint16_t)b[7] << 8));

    if (len > BLEH_FRAME_MAX)
    {
        return false;
    }

    h->type  = b[2];
    h->flags = b[3];
    h->seq   = (uint16_t)(b[4] | ((uint16_t)b[5] << 8));
    h->len   = len;
    return true;
}

void bleh_hdr_build(uint8_t *b, uint8_t type, uint8_t flags, uint16_t seq,
                    uint16_t len)
{
    b[0] = BLEH_MAGIC;
    b[1] = BLEH_VERSION;
    b[2] = type;
    b[3] = flags;
    b[4] = (uint8_t)(seq & 0xFF);
    b[5] = (uint8_t)(seq >> 8);
    b[6] = (uint8_t)(len & 0xFF);
    b[7] = (uint8_t)(len >> 8);
}

/* ---- heads ------------------------------------------------------------------------ */

static bool method_ok(const char *m)
{
    return strcmp(m, "GET") == 0 || strcmp(m, "POST") == 0 ||
           strcmp(m, "PUT") == 0 || strcmp(m, "DELETE") == 0;
}

static bool path_ok(const char *p)
{
    size_t n = strlen(p);

    if (n < strlen(BLEH_PATH_PREFIX) ||
        strncmp(p, BLEH_PATH_PREFIX, strlen(BLEH_PATH_PREFIX)) != 0)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)p[i];

        /* the app percent-encodes anything else; a space or a control
           character would corrupt the request line */
        if (c <= 0x20 || c >= 0x7F)
        {
            return false;
        }
    }

    /* no path traversal games on the loopback replay either */
    return strstr(p, "/../") == NULL && strstr(p, "//") == NULL;
}

bool bleh_req_parse(const char *json, size_t n, bleh_req_t *out, int *status,
                    const char **err)
{
    memset(out, 0, sizeof(*out));
    *status = BLEH_S_BAD_REQUEST;
    *err = "bad_request";

    cJSON *o = cJSON_ParseWithLength(json, n);

    if (!cJSON_IsObject(o))
    {
        cJSON_Delete(o);
        return false;
    }

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "m");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "p");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(o, "ct");
    const cJSON *len = cJSON_GetObjectItemCaseSensitive(o, "len");
    bool ok = false;

    if (!cJSON_IsString(m) || strlen(m->valuestring) >= BLEH_METHOD_MAX ||
        !cJSON_IsString(p) || strlen(p->valuestring) >= BLEH_PATH_MAX)
    {
        goto out;
    }

    strcpy(out->method, m->valuestring);
    strcpy(out->path, p->valuestring);

    if (!method_ok(out->method))
    {
        *err = "method";
        goto out;
    }

    if (!path_ok(out->path))
    {
        *status = BLEH_S_FORBIDDEN;
        *err = "path";
        goto out;
    }

    if (cJSON_IsString(ct))
    {
        if (strlen(ct->valuestring) >= BLEH_CT_MAX)
        {
            goto out;
        }

        strcpy(out->ct, ct->valuestring);
    }

    if (len != NULL)
    {
        if (!cJSON_IsNumber(len) || len->valuedouble < 0 ||
            len->valuedouble > (double)BLEH_BODY_MAX)
        {
            *err = "len";
            goto out;
        }

        out->len = (uint32_t)len->valuedouble;
    }

    ok = true;
    *err = NULL;

out:
    cJSON_Delete(o);
    return ok;
}

size_t bleh_rsp_head(char *dst, size_t cap, int status, const char *ct,
                     int64_t len, const char *err)
{
    int n;

    if (err != NULL)
    {
        n = snprintf(dst, cap, "{\"s\":%d,\"err\":\"%s\"}", status, err);
    }
    else
    {
        n = snprintf(dst, cap, "{\"s\":%d,\"ct\":\"%s\",\"len\":%lld}",
                     status, (ct != NULL) ? ct : "", (long long)len);
    }

    if (n < 0)
    {
        return 0;
    }

    return ((size_t)n >= cap) ? cap - 1 : (size_t)n;
}

/* ---- emit helpers ------------------------------------------------------------------- */

static bool emit_frame(bleh_core_t *c, uint8_t type, uint8_t flags,
                       uint16_t seq, const void *payload, size_t len)
{
    if (len > BLEH_FRAME_MAX)
    {
        return false;
    }

    bleh_hdr_build(c->tx, type, flags, seq, (uint16_t)len);

    if (len > 0 && payload != c->tx + BLEH_HDR_SIZE)
    {
        memcpy(c->tx + BLEH_HDR_SIZE, payload, len);
    }

    c->ctr.frames_tx++;

    if (!c->emit(c->emit_arg, c->tx, BLEH_HDR_SIZE + len))
    {
        return false;
    }

    if (type == BLEH_T_RSP_BODY)
    {
        c->ctr.bytes_out += (uint32_t)len;
    }

    return true;
}

static bool emit_error(bleh_core_t *c, uint16_t seq, int status,
                       const char *err)
{
    char head[64];
    size_t n = bleh_rsp_head(head, sizeof(head), status, NULL, 0, err);

    c->ctr.errors++;
    c->ctr.responses++;
    c->ctr.last_status = status;

    if (!emit_frame(c, BLEH_T_RSP, 0, seq, head, n))
    {
        return false;
    }

    /* every response ends with a LAST body frame, empty here (counter 0) */
    return emit_frame(c, BLEH_T_RSP_BODY, BLEH_CTR_FLAGS(0, true), seq, NULL, 0);
}

static void upstream_close(bleh_core_t *c)
{
    if (c->upstream_open)
    {
        c->ops->close(c->ops_ctx);
        c->upstream_open = false;
    }
}

static void go_idle(bleh_core_t *c)
{
    upstream_close(c);
    c->state = BLEH_ST_IDLE;
    c->received = 0;
    c->last_credit = 0;
    c->out_sent = 0;
    c->out_acked = 0;
}

/* the empty LAST frame that closes every response (its counter too) */
static bool emit_last(bleh_core_t *c)
{
    return emit_frame(c, BLEH_T_RSP_BODY, BLEH_CTR_FLAGS(c->rsp_ctr, true),
                      c->seq, NULL, 0);
}

/* ---- the forward + response stream ---------------------------------------------------- */

static void finish_request(bleh_core_t *c, uint32_t now_ms)
{
    int status = 0;
    char ct[BLEH_CT_MAX] = "";
    int64_t len = -1;

    if (c->ops->fetch(c->ops_ctx, &status, ct, sizeof(ct), &len) != ESP_OK)
    {
        emit_error(c, c->seq, BLEH_S_UPSTREAM, "upstream");
        go_idle(c);
        return;
    }

    char head[BLEH_CT_MAX + 48];
    size_t n = bleh_rsp_head(head, sizeof(head), status, ct, len, NULL);

    c->ctr.responses++;
    c->ctr.last_status = status;

    if (!emit_frame(c, BLEH_T_RSP, 0, c->seq, head, n))
    {
        go_idle(c);
        return;
    }

    /* the body streams from bleh_core_pump(): the link mode sampled at
       the REQ decides frame size and whether app CREDITs gate it */
    c->state = BLEH_ST_RESPONDING;
    c->out_sent = 0;
    c->out_acked = 0;
    c->out_stalled = false;
    c->rsp_ctr = 0;
    c->out_window = c->link_notify ? BLEH_OUT_WINDOW : 0;
    c->out_frame = BLEH_FRAME_MAX;

    if (c->link_notify)
    {
        uint16_t pdu = (c->link_pdu > BLEH_HDR_SIZE) ? c->link_pdu : 20;
        uint16_t cap = (uint16_t)(pdu - BLEH_HDR_SIZE);

        if (cap < c->out_frame)
        {
            c->out_frame = cap;
        }
    }

    c->last_activity_ms = now_ms;
}

void bleh_core_set_link(bleh_core_t *c, bool notify, uint16_t pdu_payload)
{
    c->link_notify = notify;
    c->link_pdu = pdu_payload;
}

#define BLEH_PUMP_FRAMES 16 /* per call: keeps rx/tick responsive */

bool bleh_core_pump(bleh_core_t *c, uint32_t now_ms)
{
    if (c->state != BLEH_ST_RESPONDING)
    {
        return false;
    }

    for (int k = 0; k < BLEH_PUMP_FRAMES; k++)
    {
        if (c->out_window != 0 &&
            c->out_sent - c->out_acked + c->out_frame > c->out_window)
        {
            /* the app has a window in flight: wait for its CREDIT */
            if (!c->out_stalled)
            {
                c->out_stalled = true;
                c->ctr.credit_stalls++;
            }

            return false;
        }

        int r = c->ops->read(c->ops_ctx, c->tx + BLEH_HDR_SIZE, c->out_frame);

        if (r <= 0)
        {
            /* r == 0: complete; r < 0: mid-body failure, the LAST frame
               still closes the response (the app sees fewer bytes than
               `len` promised, or a counter gap never: frames are whole) */
            (void)emit_last(c);
            go_idle(c);
            return false;
        }

        if (!emit_frame(c, BLEH_T_RSP_BODY, BLEH_CTR_FLAGS(c->rsp_ctr, false),
                        c->seq, c->tx + BLEH_HDR_SIZE, (size_t)r))
        {
            go_idle(c);
            return false;
        }

        c->rsp_ctr = (uint8_t)((c->rsp_ctr + 1) & 0x0F);
        c->out_sent += (uint32_t)r;
        c->last_activity_ms = now_ms;
    }

    return true;
}

/* ---- frame handlers ---------------------------------------------------------------------- */

static void on_req(bleh_core_t *c, const bleh_hdr_t *h, const uint8_t *payload,
                   uint32_t now_ms)
{
    if (c->state != BLEH_ST_IDLE)
    {
        /* one request in flight per link; the active one continues */
        emit_error(c, h->seq, BLEH_S_BUSY, "busy");
        return;
    }

    int status;
    const char *err;

    if (!bleh_req_parse((const char *)payload, h->len, &c->req, &status,
                        &err))
    {
        emit_error(c, h->seq, status, err);
        return;
    }

    c->ctr.requests++;
    c->seq = h->seq;
    c->received = 0;
    c->last_credit = 0;
    c->req_ctr = 0;
    c->last_activity_ms = now_ms;

    if (c->ops->open(c->ops_ctx, &c->req) != ESP_OK)
    {
        emit_error(c, h->seq, BLEH_S_UPSTREAM, "upstream");
        return;
    }

    c->upstream_open = true;

    if (c->req.len == 0)
    {
        finish_request(c, now_ms); /* bodiless: head right away */
        return;
    }

    c->state = BLEH_ST_BODY;
}

static void on_req_body(bleh_core_t *c, const bleh_hdr_t *h,
                        const uint8_t *payload, uint32_t now_ms)
{
    if (c->state != BLEH_ST_BODY || h->seq != c->seq)
    {
        return; /* stray body frame (aborted / timed-out request) */
    }

    c->last_activity_ms = now_ms;

    if (((h->flags & BLEH_F_CTR_MASK) >> BLEH_F_CTR_SHIFT) != c->req_ctr)
    {
        /* a body PDU never arrived (write-without-response dropped by a
           full buffer, or lost in the app's stack): the stream is torn */
        c->ctr.holes++;
        emit_error(c, c->seq, BLEH_S_BAD_REQUEST, "hole");
        go_idle(c);
        return;
    }

    c->req_ctr = (uint8_t)((c->req_ctr + 1) & 0x0F);

    if ((uint64_t)c->received + h->len > c->req.len)
    {
        emit_error(c, c->seq, BLEH_S_BAD_REQUEST, "len");
        go_idle(c);
        return;
    }

    if (h->len > 0 && c->ops->write(c->ops_ctx, payload, h->len) != ESP_OK)
    {
        emit_error(c, c->seq, BLEH_S_UPSTREAM, "upstream");
        go_idle(c);
        return;
    }

    c->received += h->len;
    c->ctr.bytes_in += h->len;

    bool complete = (c->received == c->req.len);

    if ((h->flags & BLEH_F_LAST) && !complete)
    {
        emit_error(c, c->seq, BLEH_S_BAD_REQUEST, "short");
        go_idle(c);
        return;
    }

    if (complete)
    {
        finish_request(c, now_ms);
        return;
    }

    if (c->received - c->last_credit >= BLEH_CREDIT_STEP)
    {
        uint8_t credit[4] =
        {
            (uint8_t)(c->received & 0xFF), (uint8_t)((c->received >> 8) & 0xFF),
            (uint8_t)((c->received >> 16) & 0xFF), (uint8_t)(c->received >> 24),
        };

        c->last_credit = c->received;

        if (!emit_frame(c, BLEH_T_CREDIT, 0, c->seq, credit, sizeof(credit)))
        {
            go_idle(c);
        }
    }
}

static void on_abort(bleh_core_t *c, const bleh_hdr_t *h)
{
    c->ctr.aborts++;

    if (h->seq != c->seq)
    {
        return;
    }

    if (c->state == BLEH_ST_BODY)
    {
        upstream_close(c);
        emit_error(c, c->seq, BLEH_S_ABORTED, "aborted");
        go_idle(c);
    }
    else if (c->state == BLEH_ST_RESPONDING)
    {
        /* the head already went: close the body early with its LAST */
        upstream_close(c);
        (void)emit_last(c);
        go_idle(c);
    }
}

static void on_credit(bleh_core_t *c, const bleh_hdr_t *h,
                      const uint8_t *payload, uint32_t now_ms)
{
    if (c->state != BLEH_ST_RESPONDING || h->seq != c->seq || h->len != 4)
    {
        return; /* a late credit of a finished response */
    }

    uint32_t v = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                 ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);

    c->ctr.credits_rx++;
    c->last_activity_ms = now_ms;

    if (v > c->out_acked && v <= c->out_sent)
    {
        c->out_acked = v; /* cumulative; never beyond what went out */
        c->out_stalled = false;
    }
}

static void handle_frame(bleh_core_t *c, const bleh_hdr_t *h,
                         const uint8_t *payload, uint32_t now_ms)
{
    c->ctr.frames_rx++;

    switch (h->type)
    {
        case BLEH_T_REQ:      on_req(c, h, payload, now_ms); break;
        case BLEH_T_REQ_BODY: on_req_body(c, h, payload, now_ms); break;
        case BLEH_T_ABORT:    on_abort(c, h); break;
        case BLEH_T_CREDIT:   on_credit(c, h, payload, now_ms); break;
        default:              break; /* RSP/RSP_BODY from the app: ignored */
    }
}

/* ---- reassembly ----------------------------------------------------------------------------- */

void bleh_core_init(bleh_core_t *c, const bleh_ops_t *ops, void *ops_ctx,
                    bleh_emit_fn emit, void *emit_arg, uint8_t *rx,
                    uint8_t *tx)
{
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->ops_ctx = ops_ctx;
    c->emit = emit;
    c->emit_arg = emit_arg;
    c->rx = rx;
    c->tx = tx;
}

void bleh_core_rx(bleh_core_t *c, const uint8_t *data, size_t len,
                  uint32_t now_ms)
{
    while (len > 0)
    {
        size_t room = BLEH_FRAME_BUF - c->rx_len;
        size_t take = (len < room) ? len : room;

        memcpy(c->rx + c->rx_len, data, take);
        c->rx_len += take;
        data += take;
        len -= take;

        for (;;)
        {
            if (c->rx_len < BLEH_HDR_SIZE)
            {
                break;
            }

            bleh_hdr_t h;

            if (!bleh_hdr_parse(c->rx, &h))
            {
                /* resync: drop one byte, look for the next magic */
                c->ctr.resync++;
                memmove(c->rx, c->rx + 1, c->rx_len - 1);
                c->rx_len--;
                continue;
            }

            size_t frame = BLEH_HDR_SIZE + h.len;

            if (c->rx_len < frame)
            {
                break; /* wait for the rest */
            }

            handle_frame(c, &h, c->rx + BLEH_HDR_SIZE, now_ms);

            memmove(c->rx, c->rx + frame, c->rx_len - frame);
            c->rx_len -= frame;
        }
    }
}

void bleh_core_tick(bleh_core_t *c, uint32_t now_ms)
{
    if ((uint32_t)(now_ms - c->last_activity_ms) <= BLEH_IDLE_TIMEOUT_MS)
    {
        return;
    }

    if (c->state == BLEH_ST_BODY)
    {
        c->ctr.timeouts++;
        upstream_close(c);
        emit_error(c, c->seq, BLEH_S_TIMEOUT, "timeout");
        go_idle(c);
    }
    else if (c->state == BLEH_ST_RESPONDING)
    {
        /* the app stopped sending CREDITs (notify mode): truncate with
           the LAST frame; the app sees fewer bytes than `len` */
        c->ctr.timeouts++;
        upstream_close(c);
        (void)emit_last(c);
        go_idle(c);
    }
}

void bleh_core_link_down(bleh_core_t *c)
{
    go_idle(c);
    c->rx_len = 0;
}
