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
 * @file autopid_resp_filter.c
 * @brief PURE: ATMA monitor lines -> frame data bytes (host-tested). One
 *        monitor line that carries the wanted frame id (ap_filter_frame)
 *        and the incremental stream collector the filter window feeds
 *        with raw chip chunks (ap_flt_stream_*). Expressions index from
 *        B0 = first DATA byte. Split out of autopid_resp.c 2026-10-02
 *        (700-line rule); behaviour unchanged.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---- ATMA monitor lines (filters, Phase 4) -------------------------------------- */

static bool hex_byte(const char *s, size_t len, uint8_t *out)
{
    if (len != 2 || !isxdigit((unsigned char)s[0]) ||
        !isxdigit((unsigned char)s[1]))
    {
        return false;
    }

    char tmp[3] = { s[0], s[1], '\0' };

    *out = (uint8_t)strtol(tmp, NULL, 16);
    return true;
}

bool ap_filter_frame(const char *line, size_t len, uint32_t frame_id,
                     uint8_t *payload, size_t payload_max,
                     size_t *out_len)
{
    *out_len = 0;

    /* tokenize into up to 16 whitespace-separated tokens */
    struct { const char *s; size_t len; } tok[16];
    size_t n_tok = 0, pos = 0;

    while (pos < len && n_tok < 16)
    {
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        {
            pos++;
        }

        size_t start = pos;

        while (pos < len && line[pos] != ' ' && line[pos] != '\t')
        {
            pos++;
        }

        if (pos > start)
        {
            tok[n_tok].s = &line[start];
            tok[n_tok].len = pos - start;
            n_tok++;
        }
    }

    if (n_tok == 0)
    {
        return false;
    }

    /* header (legacy-compatible shapes): contiguous 3-hex (11-bit) or
       8-hex (29-bit) first token, or the id split into 2-hex byte
       tokens (4 for extended, 2 for standard-with-leading-zero) */
    size_t data_from = 0;
    bool matched = false;

    if (tok[0].len == 3 || tok[0].len == 8)
    {
        bool hex = true;

        for (size_t i = 0; i < tok[0].len; i++)
        {
            if (!isxdigit((unsigned char)tok[0].s[i]))
            {
                hex = false;
                break;
            }
        }

        if (hex)
        {
            char tmp[9] = { 0 };

            memcpy(tmp, tok[0].s, tok[0].len);
            matched = ((uint32_t)strtoul(tmp, NULL, 16) == frame_id);
            data_from = 1;
        }
    }
    else if (tok[0].len == 2)
    {
        uint8_t b[4];

        if (n_tok >= 4 && hex_byte(tok[0].s, 2, &b[0]) &&
            hex_byte(tok[1].s, 2, &b[1]) &&
            hex_byte(tok[2].s, 2, &b[2]) &&
            hex_byte(tok[3].s, 2, &b[3]) &&
            (((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
             ((uint32_t)b[2] << 8) | b[3]) == frame_id)
        {
            matched = true;     /* 29-bit id as four byte tokens         */
            data_from = 4;
        }
        else if (n_tok >= 2 && frame_id <= 0x7FF &&
                 hex_byte(tok[0].s, 2, &b[0]) &&
                 hex_byte(tok[1].s, 2, &b[1]) &&
                 (((uint32_t)b[0] << 8) | b[1]) == frame_id)
        {
            matched = true;     /* 11-bit id as two byte tokens          */
            data_from = 2;
        }
    }

    if (!matched)
    {
        return false;
    }

    /* remaining byte tokens = the frame's data (expressions index from
       B0 = first data byte — the legacy filter frame-of-reference).
       Non-hex tokens are SKIPPED like legacy did: the chip appends
       markers such as "<DATA ERROR" after byte-perfect data
       (bench-observed with PCAN-injected frames) */
    size_t n = 0;

    for (size_t i = data_from; i < n_tok && n < payload_max; i++)
    {
        uint8_t b;

        if (tok[i].len == 1 && tok[i].s[0] == '>')
        {
            break;
        }

        if (hex_byte(tok[i].s, tok[i].len, &b))
        {
            payload[n++] = b;
        }
    }

    if (n == 0)
    {
        return false;           /* header-only line (RTR/noise)          */
    }

    *out_len = n;
    return true;
}

void ap_flt_stream_init(ap_flt_stream_t *st)
{
    st->n = 0;
    st->overflow = false;
}

bool ap_flt_stream_feed_ex(ap_flt_stream_t *st, const uint8_t *bytes,
                           size_t len, uint32_t frame_id,
                           uint8_t *payload, size_t payload_max,
                           size_t *out_len, size_t *consumed)
{
    for (size_t i = 0; i < len; i++)
    {
        char ch = (char)bytes[i];

        if (ch == '\r' || ch == '\n')
        {
            /* a truncated line can't be trusted as a frame — skip it
               whole rather than parse half its bytes */
            if (st->n > 0 && !st->overflow &&
                ap_filter_frame(st->line, st->n, frame_id, payload,
                                payload_max, out_len))
            {
                st->n = 0;
                st->overflow = false;
                *consumed = i + 1;
                return true;
            }

            st->n = 0;
            st->overflow = false;
        }
        else if (st->n < sizeof(st->line) - 1)
        {
            st->line[st->n++] = ch;
        }
        else
        {
            st->overflow = true;
        }
    }

    *consumed = len;
    return false;
}

bool ap_flt_stream_feed(ap_flt_stream_t *st, const uint8_t *bytes,
                        size_t len, uint32_t frame_id, uint8_t *payload,
                        size_t payload_max, size_t *out_len)
{
    size_t consumed;

    return ap_flt_stream_feed_ex(st, bytes, len, frame_id, payload,
                                 payload_max, out_len, &consumed);
}
