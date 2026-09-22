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
 * @file ble_central_bench_tunnel.c
 * @brief Pure HTTP-over-BLE client codec + state machine (see the header).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_central_bench_tunnel.h"

#define MAGIC 0x57u
#define VER   BCBT_VERSION

void bcbt_hdr_build(uint8_t *b, uint8_t type, uint8_t flags, uint16_t seq,
                    uint16_t len)
{
    b[0] = MAGIC;
    b[1] = VER;
    b[2] = type;
    b[3] = flags;
    b[4] = (uint8_t)(seq & 0xFF);
    b[5] = (uint8_t)(seq >> 8);
    b[6] = (uint8_t)(len & 0xFF);
    b[7] = (uint8_t)(len >> 8);
}

bool bcbt_hdr_parse(const uint8_t *b, uint8_t *type, uint8_t *flags,
                    uint16_t *seq, uint16_t *len)
{
    if (b[0] != MAGIC || b[1] != VER || b[2] < BCBT_T_REQ || b[2] > BCBT_T_CREDIT)
    {
        return false;
    }

    uint16_t l = (uint16_t)(b[6] | (b[7] << 8));

    if (l > BCBT_FRAME_MAX)
    {
        return false;
    }

    *type = b[2];
    *flags = b[3];
    *seq = (uint16_t)(b[4] | (b[5] << 8));
    *len = l;
    return true;
}

size_t bcbt_req_head(char *dst, size_t cap, const char *method,
                     const char *path, const char *ct, uint32_t body_len)
{
    int n;

    if (ct != NULL && ct[0] != '\0')
    {
        n = snprintf(dst, cap, "{\"m\":\"%s\",\"p\":\"%s\",\"ct\":\"%s\",\"len\":%lu}",
                     method, path, ct, (unsigned long)body_len);
    }
    else
    {
        n = snprintf(dst, cap, "{\"m\":\"%s\",\"p\":\"%s\",\"len\":%lu}",
                     method, path, (unsigned long)body_len);
    }

    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* a tiny "find integer after key" for the two fields the client needs;
   the response head is device-generated and flat */
static bool json_int(const char *json, size_t n, const char *key, int64_t *out)
{
    char pat[16];
    int  pl = snprintf(pat, sizeof(pat), "\"%s\":", key);

    if (pl <= 0)
    {
        return false;
    }

    for (size_t i = 0; i + (size_t)pl <= n; i++)
    {
        if (memcmp(json + i, pat, (size_t)pl) == 0)
        {
            const char *p = json + i + pl;
            size_t left = n - i - (size_t)pl;
            char buf[24];
            size_t k = 0;

            while (k < left && k < sizeof(buf) - 1 &&
                   (p[k] == '-' || (p[k] >= '0' && p[k] <= '9')))
            {
                buf[k] = p[k];
                k++;
            }

            if (k == 0)
            {
                return false;
            }

            buf[k] = '\0';
            *out = strtoll(buf, NULL, 10);
            return true;
        }
    }

    return false;
}

bool bcbt_rsp_head_parse(const char *json, size_t n, int *status, int64_t *len)
{
    int64_t s;

    if (!json_int(json, n, "s", &s))
    {
        return false;
    }

    *status = (int)s;
    *len = -1;
    (void)json_int(json, n, "len", len);
    return true;
}

/* ---- client FSM ---------------------------------------------------------------- */

void bcbt_client_begin(bcbt_client_t *c, uint16_t seq)
{
    memset(c, 0, sizeof(*c));
    c->seq = seq;
    c->rsp_len = -1;
}

size_t bcbt_client_feed(bcbt_client_t *c, const uint8_t *data, size_t n,
                        bcbt_event_fn on_event, void *arg)
{
    size_t frames = 0;

    while (n > 0)
    {
        /* fill the reassembly buffer up to what the header says */
        size_t need = c->rx_len < BCBT_HDR ? BCBT_HDR : 0;

        if (c->rx_len >= BCBT_HDR)
        {
            uint8_t t, f;
            uint16_t s, l;

            if (!bcbt_hdr_parse(c->rx, &t, &f, &s, &l))
            {
                /* resync: drop one byte, look for the next magic */
                memmove(c->rx, c->rx + 1, c->rx_len - 1);
                c->rx_len--;
                c->resync++;
                continue;
            }

            need = BCBT_HDR + l;
        }

        size_t take = need - c->rx_len;

        if (take > n)
        {
            take = n;
        }

        memcpy(c->rx + c->rx_len, data, take);
        c->rx_len += take;
        data += take;
        n -= take;

        if (c->rx_len < BCBT_HDR)
        {
            continue;
        }

        uint8_t t, f;
        uint16_t s, l;

        if (!bcbt_hdr_parse(c->rx, &t, &f, &s, &l))
        {
            continue; /* the loop above resyncs */
        }

        if (c->rx_len < (size_t)BCBT_HDR + l)
        {
            continue; /* payload incomplete */
        }

        /* one complete frame */
        const uint8_t *pl = c->rx + BCBT_HDR;

        frames++;

        if (s != c->seq)
        {
            on_event(arg, BCBT_EV_STRAY, pl, l, t);
        }
        else if (t == BCBT_T_CREDIT && l == 4)
        {
            c->credited = (uint32_t)pl[0] | ((uint32_t)pl[1] << 8) |
                          ((uint32_t)pl[2] << 16) | ((uint32_t)pl[3] << 24);
            on_event(arg, BCBT_EV_CREDIT, NULL, 0, c->credited);
        }
        else if (t == BCBT_T_RSP)
        {
            int st;
            int64_t len;

            c->rsp_seen = bcbt_rsp_head_parse((const char *)pl, l, &st, &len);
            c->status = c->rsp_seen ? st : 0;
            c->rsp_len = len;
            on_event(arg, BCBT_EV_RSP, pl, l, (uint32_t)c->status);
        }
        else if (t == BCBT_T_RSP_BODY)
        {
            uint8_t ctr = (uint8_t)(f >> BCBT_F_CTR_SHIFT);

            if (ctr != c->body_ctr)
            {
                /* a whole frame (= a notification PDU) is missing */
                c->holes++;
                on_event(arg, BCBT_EV_HOLE, NULL, 0, ctr);
                c->body_ctr = ctr; /* resume counting from what arrived */
            }

            c->body_ctr = (uint8_t)((c->body_ctr + 1) & 0x0F);

            if (l > 0)
            {
                c->body_rx += l;
                on_event(arg, BCBT_EV_BODY, pl, l, 0);
            }

            if (f & BCBT_F_LAST)
            {
                c->done = true;
                on_event(arg, BCBT_EV_DONE, NULL, 0, 0);
            }
        }
        else
        {
            on_event(arg, BCBT_EV_STRAY, pl, l, t);
        }

        c->rx_len = 0;
    }

    return frames;
}

bool bcbt_client_may_send(const bcbt_client_t *c, uint32_t sent_total)
{
    return sent_total - c->credited < BCBT_CREDIT_WIN;
}

bool bcbt_client_credit_due(const bcbt_client_t *c, uint32_t step)
{
    return c->body_rx - c->credit_sent >= step && !c->done;
}

size_t bcbt_client_credit_frame(bcbt_client_t *c, uint8_t *dst)
{
    uint32_t v = c->body_rx;

    bcbt_hdr_build(dst, BCBT_T_CREDIT, 0, c->seq, 4);
    dst[BCBT_HDR + 0] = (uint8_t)(v & 0xFF);
    dst[BCBT_HDR + 1] = (uint8_t)((v >> 8) & 0xFF);
    dst[BCBT_HDR + 2] = (uint8_t)((v >> 16) & 0xFF);
    dst[BCBT_HDR + 3] = (uint8_t)(v >> 24);
    c->credit_sent = v;
    return BCBT_HDR + 4;
}
