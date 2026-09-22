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
 * @file ble_central_bench_modes.c
 * @brief The timed tests, run on the bench task: notify (the peer blasts
 *        its data pipe, we count), write (write-without-response as fast
 *        as the stack takes it), read (DIS loop), and the HTTP-over-BLE
 *        client runner for tunnel_up / tunnel_down (byte-exact against a
 *        deterministic payload). Only the host task's FFF1 byte counter
 *        is touched from another context.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "ble_central_bench_private.h"
#include "ble_central_bench_tunnel.h"

/* notify-mode accounting written from the host task */
static volatile uint32_t s_nt_bytes, s_nt_count;
static volatile int64_t  s_nt_first_us, s_nt_last_us;
static volatile bool     s_nt_armed;

void bcb_modes_on_fff1(size_t n)
{
    if (!s_nt_armed)
    {
        return;
    }

    int64_t now = esp_timer_get_time();

    if (s_nt_first_us == 0)
    {
        s_nt_first_us = now;
    }

    s_nt_last_us = now;
    s_nt_bytes += n;
    s_nt_count++;
}

/* ---- the deterministic payload (= the Pi bench's rand_blob(size, seed)) --------- */

typedef struct { uint32_t x; uint32_t word; int byte; } pat_t;

static void pat_init(pat_t *p, uint32_t seed)
{
    p->x = seed;
    p->byte = 4;               /* forces a new word on the first byte */
    p->word = 0;
}

static uint8_t pat_next(pat_t *p)
{
    if (p->byte == 4)
    {
        p->x = p->x * 1103515245u + 12345u;
        p->word = p->x;
        p->byte = 0;
    }

    return (uint8_t)(p->word >> (8 * p->byte++));
}

static void pat_fill(pat_t *p, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        dst[i] = pat_next(p);
    }
}

/* ---- the HTTP-over-BLE client on FFF3/FFF4 ------------------------------------- */

typedef struct
{
    bcbt_client_t cl;
    pat_t         expect;      /* tunnel_down: the payload we expect         */
    uint32_t      body_bytes;
    bool          mismatch;
    int64_t       first_body_us, last_body_us;
    bool          notify;      /* FFF3 subscribed for notifications: CREDIT  */
    uint32_t      credits;     /* CREDIT frames sent                         */
} tun_t;

static uint8_t s_out = BCB_OUT_NOTIFY; /* the run's FFF3 subscription     */

static void tun_event(void *arg, bcbt_event_t ev, const uint8_t *p, size_t n, uint32_t v)
{
    tun_t *t = arg;

    if (ev == BCBT_EV_BODY)
    {
        int64_t now = esp_timer_get_time();

        if (t->body_bytes == 0)
        {
            t->first_body_us = now;
        }

        t->last_body_us = now;

        for (size_t i = 0; i < n; i++)
        {
            if (p[i] != pat_next(&t->expect))
            {
                t->mismatch = true;
            }
        }

        t->body_bytes += n;
    }
    else if (ev == BCBT_EV_STRAY)
    {
        bcb_core_lock();
        bcb_core_status()->counters.tunnel_resync++;
        bcb_core_unlock();
    }
    else if (ev == BCBT_EV_HOLE)
    {
        bcb_diag("tunnel hole: RSP_BODY counter %lu unexpected after %lu B",
                 (unsigned long)v, (unsigned long)t->body_bytes);
    }
}

static bool tun_send(const uint8_t *frame, size_t n, bcb_result_t *r);

/* notify mode: pay the device a CREDIT for the body bytes consumed so far
   (every BCBT_CREDIT_STEP); nothing to do in indicate mode */
static void tun_credit(tun_t *t, bcb_result_t *r)
{
    if (!t->notify || !bcbt_client_credit_due(&t->cl, BCBT_CREDIT_STEP))
    {
        return;
    }

    uint8_t frame[BCBT_HDR + 4];
    size_t n = bcbt_client_credit_frame(&t->cl, frame);

    if (tun_send(frame, n, r))
    {
        t->credits++;
    }
}

/* write one frame to FFF4 in MTU-sized pieces (the channel reassembles) */
static bool tun_send(const uint8_t *frame, size_t n, bcb_result_t *r)
{
    size_t cap = bcb_payload_cap(bcb_gap_mtu());
    uint16_t fff4;

    bcb_core_lock();
    fff4 = bcb_core_status()->chars.fff4;
    bcb_core_status()->counters.tunnel_frames_tx++;
    bcb_core_unlock();

    for (size_t off = 0; off < n; off += cap)
    {
        size_t k = n - off < cap ? n - off : cap;
        int retries = bcb_gap_write_nr(fff4, frame + off, k, 2000);

        if (retries < 0)
        {
            r->errors++;
            return false;
        }

        r->retries += (uint32_t)retries;
    }

    return true;
}

/* pump FFF3 bytes into the client until `pred` holds or the deadline */
static bool tun_pump(tun_t *t, bool (*pred)(const tun_t *), int64_t deadline_us,
                     bcb_result_t *r)
{
    static uint8_t slice[512];

    while (!pred(t))
    {
        int64_t left = deadline_us - esp_timer_get_time();

        if (left <= 0)
        {
            return false;
        }

        size_t n = xStreamBufferReceive(bcb_core_http_rx(), slice, sizeof(slice),
                                        pdMS_TO_TICKS(left / 1000 < 100 ? left / 1000 : 100));

        if (n > 0)
        {
            size_t frames = bcbt_client_feed(&t->cl, slice, n, tun_event, t);

            bcb_core_lock();
            bcb_core_status()->counters.tunnel_frames_rx += frames;
            bcb_core_unlock();
            tun_credit(t, r);
        }

        if (!bcb_gap_connected())
        {
            return false;
        }
    }

    return true;
}

static bool pred_done(const tun_t *t)   { return t->cl.done; }

/** One request: head, optional body (pattern), then the response. Fills
 *  r->http_status, r->bytes/ms/kbps for the body phase (upload: our body;
 *  download: the response body), r->exact for downloads. */
static bool tun_request(const char *method, const char *path, const char *ct,
                        uint32_t body_len, uint32_t seed, uint32_t timeout_s,
                        bcb_result_t *r, tun_t *t)
{
    static uint16_t seq;
    static uint8_t frame[BCBT_HDR + BCBT_FRAME_MAX];
    char head[BCBT_HEAD_MAX];
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000LL;

    xStreamBufferReset(bcb_core_http_rx());
    memset(t, 0, sizeof(*t));
    t->notify = (s_out == BCB_OUT_NOTIFY);
    pat_init(&t->expect, seed);
    bcbt_client_begin(&t->cl, ++seq);

    size_t hn = bcbt_req_head(head, sizeof(head), method, path, ct, body_len);

    if (hn == 0)
    {
        snprintf(r->detail, sizeof(r->detail), "head too long");
        return false;
    }

    bcbt_hdr_build(frame, BCBT_T_REQ, 0, seq, (uint16_t)hn);
    memcpy(frame + BCBT_HDR, head, hn);

    if (!tun_send(frame, BCBT_HDR + hn, r))
    {
        snprintf(r->detail, sizeof(r->detail), "head write failed");
        return false;
    }

    /* body (upload) with the credit window */
    pat_t body;
    uint32_t sent = 0;
    uint8_t ctr = 0;
    int64_t t0 = esp_timer_get_time();

    pat_init(&body, seed);

    while (sent < body_len)
    {
        if (!bcbt_client_may_send(&t->cl, sent))
        {
            /* wait for a CREDIT (or an early response) */
            uint32_t before = t->cl.credited;
            int64_t d2 = esp_timer_get_time() + 5000000LL;

            while (t->cl.credited == before && !t->cl.rsp_seen)
            {
                uint8_t slice[256];
                size_t n = xStreamBufferReceive(bcb_core_http_rx(), slice, sizeof(slice), pdMS_TO_TICKS(50));

                if (n > 0)
                {
                    bcbt_client_feed(&t->cl, slice, n, tun_event, t);
                    tun_credit(t, r);
                }

                if (esp_timer_get_time() > d2 || !bcb_gap_connected())
                {
                    snprintf(r->detail, sizeof(r->detail), "credit timeout at %lu", (unsigned long)sent);
                    r->errors++;
                    return false;
                }
            }

            if (t->cl.rsp_seen)
            {
                break; /* the device answered early (an error status) */
            }
        }

        uint32_t k = body_len - sent < BCBT_FRAME_MAX ? body_len - sent : BCBT_FRAME_MAX;
        bool last = sent + k == body_len;

        bcbt_hdr_build(frame, BCBT_T_REQ_BODY, BCBT_CTR_FLAGS(ctr, last), seq, (uint16_t)k);
        ctr = (uint8_t)((ctr + 1) & 0x0F);
        pat_fill(&body, frame + BCBT_HDR, k);

        if (!tun_send(frame, BCBT_HDR + k, r))
        {
            snprintf(r->detail, sizeof(r->detail), "body write failed at %lu", (unsigned long)sent);
            return false;
        }

        sent += k;
    }

    int64_t t_body = esp_timer_get_time() - t0;

    if (!tun_pump(t, pred_done, deadline, r))
    {
        snprintf(r->detail, sizeof(r->detail), "response timeout (rsp %s, %lu body B)",
                 t->cl.rsp_seen ? "seen" : "none", (unsigned long)t->body_bytes);
        r->errors++;
        return false;
    }

    r->http_status = t->cl.status;
    r->holes = t->cl.holes;
    r->credits = t->credits;
    r->out = s_out;

    if (body_len > 0)
    {
        r->bytes = sent;
        r->ms = (uint32_t)(t_body / 1000);
    }
    else
    {
        r->bytes = t->body_bytes;
        r->ms = (uint32_t)((t->last_body_us - t->first_body_us) / 1000);
        r->exact = !t->mismatch;
    }

    r->kbps = r->ms ? (uint32_t)((uint64_t)r->bytes * 8u / r->ms) : 0;
    return true;
}

/* ---- the modes ------------------------------------------------------------------- */

void bcb_mode_notify(uint32_t seconds, uint32_t size, bcb_result_t *r)
{
    bcb_chars_t ch;
    tun_t t;
    char body[64];

    bcb_core_lock();
    ch = bcb_core_status()->chars;
    bcb_core_unlock();

    if (ch.fff1_cccd == 0 || ch.fff4 == 0)
    {
        snprintf(r->detail, sizeof(r->detail), "FFF1 or the http channel missing");
        return;
    }

    s_out = BCB_OUT_NOTIFY;

    if (bcb_gap_subscribe(ch.fff1_cccd, 1) != ESP_OK ||
        (ch.fff3_cccd && bcb_gap_subscribe(ch.fff3_cccd, 1) != ESP_OK))
    {
        snprintf(r->detail, sizeof(r->detail), "subscribe failed");
        return;
    }

    s_nt_bytes = 0;
    s_nt_count = 0;
    s_nt_first_us = 0;
    s_nt_last_us = 0;
    s_nt_armed = true;

    /* ask the peer to blast `size` bytes on its data pipe */
    snprintf(body, sizeof(body), "{\"bytes\":%lu}", (unsigned long)size);

    bcb_result_t rr = { 0 };
    size_t bl = strlen(body);
    static uint8_t frame[BCBT_HDR + 64];
    static uint16_t seq = 0x4000;
    char head[BCBT_HEAD_MAX];

    xStreamBufferReset(bcb_core_http_rx());
    memset(&t, 0, sizeof(t));
    bcbt_client_begin(&t.cl, ++seq);

    size_t hn = bcbt_req_head(head, sizeof(head), "POST", "/api/ble/blast",
                              "application/json", (uint32_t)bl);

    bcbt_hdr_build(frame, BCBT_T_REQ, 0, seq, (uint16_t)hn);
    memcpy(frame + BCBT_HDR, head, hn);
    (void)tun_send(frame, BCBT_HDR + hn, &rr);
    bcbt_hdr_build(frame, BCBT_T_REQ_BODY, BCBT_F_LAST, seq, (uint16_t)bl);
    memcpy(frame + BCBT_HDR, body, bl);
    (void)tun_send(frame, BCBT_HDR + bl, &rr);

    if (!tun_pump(&t, pred_done, esp_timer_get_time() + 10000000LL, r))
    {
        snprintf(r->detail, sizeof(r->detail), "blast request: no response");
        r->errors++;
        s_nt_armed = false;
        return;
    }

    r->http_status = t.cl.status;

    if (t.cl.status != 200)
    {
        snprintf(r->detail, sizeof(r->detail), "blast refused (%d)", t.cl.status);
        s_nt_armed = false;
        return;
    }

    /* count until the bytes arrived or the time is up */
    int64_t deadline = esp_timer_get_time() + (int64_t)seconds * 1000000LL;

    while (s_nt_bytes < size && esp_timer_get_time() < deadline && bcb_gap_connected())
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_nt_armed = false;
    r->bytes = s_nt_bytes;
    r->count = s_nt_count;
    r->ms = (uint32_t)((s_nt_last_us - s_nt_first_us) / 1000);
    r->kbps = r->ms ? (uint32_t)((uint64_t)r->bytes * 8u / r->ms) : 0;
    r->ok = r->bytes >= size;

    if (!r->ok)
    {
        snprintf(r->detail, sizeof(r->detail), "%lu of %lu B in %lu s",
                 (unsigned long)r->bytes, (unsigned long)size, (unsigned long)seconds);
    }
}

void bcb_mode_write(uint32_t seconds, bcb_result_t *r)
{
    static uint8_t buf[512];
    bcb_chars_t ch;
    size_t cap = bcb_payload_cap(bcb_gap_mtu());

    bcb_core_lock();
    ch = bcb_core_status()->chars;
    bcb_core_unlock();

    if (ch.fff2 == 0)
    {
        snprintf(r->detail, sizeof(r->detail), "FFF2 missing");
        return;
    }

    if (cap > sizeof(buf))
    {
        cap = sizeof(buf);
    }

    for (size_t i = 0; i < cap; i++)
    {
        buf[i] = (uint8_t)i;
    }

    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)seconds * 1000000LL;

    while (esp_timer_get_time() < deadline && bcb_gap_connected())
    {
        int retries = bcb_gap_write_nr(ch.fff2, buf, cap, 2000);

        if (retries < 0)
        {
            r->errors++;
            break;
        }

        r->retries += (uint32_t)retries;
        r->count++;
        r->bytes += cap;
    }

    r->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    r->kbps = r->ms ? (uint32_t)((uint64_t)r->bytes * 8u / r->ms) : 0;
    r->ok = r->errors == 0 && r->count > 0;

    bcb_core_lock();
    bcb_core_status()->counters.write_tx += r->count;
    bcb_core_status()->counters.write_bytes += r->bytes;
    bcb_core_unlock();
}

void bcb_mode_read(uint32_t seconds, bcb_result_t *r)
{
    uint8_t buf[64];
    bcb_chars_t ch;

    bcb_core_lock();
    ch = bcb_core_status()->chars;
    bcb_core_unlock();

    if (ch.dis_mfr == 0)
    {
        snprintf(r->detail, sizeof(r->detail), "DIS 2A29 missing");
        return;
    }

    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)seconds * 1000000LL;

    while (esp_timer_get_time() < deadline && bcb_gap_connected())
    {
        int n = bcb_gap_read(ch.dis_mfr, buf, sizeof(buf));

        if (n < 0)
        {
            r->errors++;
            break;
        }

        r->count++;
        r->bytes += (uint32_t)n;
    }

    r->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    r->kbps = r->ms ? (uint32_t)((uint64_t)r->bytes * 8u / r->ms) : 0;
    r->ok = r->errors == 0 && r->count > 0;
}

void bcb_mode_tunnel(bcb_mode_t mode, uint32_t seconds, uint32_t size, uint8_t out,
                     bcb_result_t *r)
{
    bcb_chars_t ch;
    tun_t t;

    s_out = (out == BCB_OUT_INDICATE) ? BCB_OUT_INDICATE : BCB_OUT_NOTIFY;
    r->out = s_out;

    bcb_core_lock();
    ch = bcb_core_status()->chars;
    bcb_core_unlock();

    if (ch.fff3_cccd == 0 || ch.fff4 == 0)
    {
        snprintf(r->detail, sizeof(r->detail), "http channel missing");
        return;
    }

    /* CCCD 0x0001 = notifications (ble_http v2: we owe CREDITs, the
       counter shows a lost PDU), 0x0002 = indications (confirmed, slow) */
    if (bcb_gap_subscribe(ch.fff3_cccd, s_out == BCB_OUT_NOTIFY ? 1 : 2) != ESP_OK)
    {
        snprintf(r->detail, sizeof(r->detail), "subscribe FFF3 failed");
        return;
    }

    if (mode == BCB_MODE_TUNNEL_UP)
    {
        if (!tun_request("POST", "/api/fs/mkdir?path=/data/blebench", NULL, 0, 0, 10, r, &t))
        {
            return;
        }

        r->ok = tun_request("POST", "/api/fs/upload?path=/data/blebench/blob.bin",
                            "application/octet-stream", size, size, seconds, r, &t) &&
                r->http_status == 200;
    }
    else
    {
        r->ok = tun_request("GET", "/api/fs/download?path=/data/blebench/blob.bin", NULL,
                            0, size, seconds, r, &t) &&
                r->http_status == 200 && r->exact && r->bytes == size && r->holes == 0;

        if (!r->ok && r->detail[0] == '\0')
        {
            snprintf(r->detail, sizeof(r->detail), "status %d, %lu B, exact %d, holes %lu",
                     r->http_status, (unsigned long)r->bytes, r->exact,
                     (unsigned long)r->holes);
        }
    }
}

