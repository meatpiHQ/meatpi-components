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
 * @file autopid_resp.c
 * @brief PURE: ELM327 response text -> payload bytes (host-tested).
 *
 * Input is exactly what obd_chip_request() returns (echo + prompt already
 * stripped; '\r'/'\n' line endings preserved). Output is the byte payload
 * user expressions index into — INCLUDING the service/PID echo bytes
 * (B0 = 0x41 for a mode-01 response), matching the legacy evaluator's
 * frame-of-reference so published profile expressions keep working.
 *
 * Accepted shapes (auto-detected per line):
 *   41 0C 1A F8                headers off, single frame
 *   014 / 0: 49 02 01 .. / 1:  headers off, ISO-TP multi-line (length,
 *                              then indexed 8-byte rows; trimmed to length)
 *   7E8 06 41 00 BE 7F B8 13   headers on, single frame (PCI 0x0L)
 *   7E8 10 14 49 02 01 ..      headers on, ISO-TP first (0x1L LL) +
 *   7E8 21 ..                  consecutive (0x2N) — reassembled from the
 *                              LOWEST responder ID only (legacy rule)
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define AP_LINE_MAX  128
#define AP_MAX_LINES 64

typedef struct
{
    uint8_t bytes[AP_LINE_MAX / 2];
    size_t  n;
    uint32_t header;      /* first token when it isn't a data byte       */
    bool     has_header;
    int      iso_index;   /* "N:" row index, -1 = none                    */
    long     bare_value;  /* whole line was ONE hex token (length line)   */
    bool     is_bare;
} ap_line_t;

/* The line table is ~5.6 KB — far too big for a task stack (a 6144-byte
   job stack silently overflowed on it, 2026-07-22; the 2026-07-22 static
   stack audit then showed EVERY caller was budgeting for it). One shared
   PSRAM table behind a mutex instead: response assembly is a
   parse-after-transaction step and the chip transactions are already
   serialized, so the lock never contends in practice. */
static ap_line_t s_lines[AP_MAX_LINES] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_lines_lock;
static StaticSemaphore_t s_lines_lock_buf;

static void lines_lock(void)
{
    /* lazy create: first callers (boot provisioning, host tests) run
       single-threaded, every later caller is past init */
    if (s_lines_lock == NULL)
    {
        s_lines_lock = xSemaphoreCreateMutexStatic(&s_lines_lock_buf);
    }

    xSemaphoreTake(s_lines_lock, portMAX_DELAY);
}

static void lines_unlock(void)
{
    xSemaphoreGive(s_lines_lock);
}

static bool is_hex_str(const char *s, size_t len)
{
    if (len == 0)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
        {
            return false;
        }
    }

    return true;
}

/* Tokenize one line into hex bytes / header / iso-row-index. */
static bool parse_line(const char *line, size_t len, ap_line_t *out)
{
    memset(out, 0, sizeof(*out));
    out->iso_index = -1;

    char tok[AP_LINE_MAX];
    size_t pos = 0;
    int n_tok = 0;

    while (pos < len)
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

        size_t tlen = pos - start;

        if (tlen == 0)
        {
            continue;
        }

        if (tlen >= sizeof(tok))
        {
            return false;
        }

        memcpy(tok, &line[start], tlen);
        tok[tlen] = '\0';

        /* "N:" ISO-TP row marker (first token only) */
        if (n_tok == 0 && tlen >= 2 && tok[tlen - 1] == ':' &&
            is_hex_str(tok, tlen - 1))
        {
            out->iso_index = (int)strtol(tok, NULL, 16);
            n_tok++;
            continue;
        }

        if (!is_hex_str(tok, tlen))
        {
            return false;
        }

        if (tlen == 2 || (tlen == 1 && n_tok > 0))
        {
            if (out->n >= sizeof(out->bytes))
            {
                return false;
            }

            out->bytes[out->n++] = (uint8_t)strtol(tok, NULL, 16);
        }
        else if (n_tok == 0)
        {
            /* first token, not a byte: CAN header (3 or 8 hex) or a bare
               ISO-TP length line ("014") */
            if (tlen == 3 || tlen == 8)
            {
                out->header = (uint32_t)strtoul(tok, NULL, 16);
                out->has_header = true;
            }

            out->bare_value = strtol(tok, NULL, 16);
            out->is_bare = true; /* provisional: bare iff no bytes follow */
        }
        else
        {
            return false; /* long hex token mid-line */
        }

        n_tok++;
    }

    if (out->n > 0 || out->iso_index >= 0)
    {
        out->is_bare = false;
    }

    return (out->n > 0) || out->is_bare || out->iso_index >= 0;
}

static bool line_is_noise(const char *line, size_t len)
{
    static const char *const NOISE[] =
    {
        "SEARCHING", "BUS INIT", "OK", "STOPPED",
    };

    for (size_t i = 0; i < sizeof(NOISE) / sizeof(NOISE[0]); i++)
    {
        if (strncmp(line, NOISE[i], strlen(NOISE[i])) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool line_is_error(const char *line, size_t len)
{
    static const char *const ERR[] =
    {
        "NO DATA", "ERROR", "CAN ERROR", "UNABLE TO CONNECT", "?",
    };

    for (size_t i = 0; i < sizeof(ERR) / sizeof(ERR[0]); i++)
    {
        if (strncmp(line, ERR[i], strlen(ERR[i])) == 0)
        {
            return true;
        }
    }

    return false;
}

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

bool ap_payload_matches_cmd(const char *cmd, const uint8_t *payload,
                            size_t payload_len)
{
    /* parse the leading hex byte pairs of the command ("010C", "22202A",
       "03"; a trailing response-count hint digit like "010C1" is ignored
       by pair alignment). Non-hex commands (AT/ST/VT) can't be checked. */
    uint8_t want[2];
    size_t n_want = 0;
    const char *p = cmd;

    while (n_want < 2)
    {
        while (*p == ' ')
        {
            p++;
        }

        if (!isxdigit((unsigned char)p[0]) ||
            !isxdigit((unsigned char)p[1]))
        {
            break;
        }

        char pair[3] = { p[0], p[1], '\0' };

        want[n_want++] = (uint8_t)strtol(pair, NULL, 16);
        p += 2;
    }

    if (n_want == 0)
    {
        return true; /* AT/ST/VT — nothing to verify */
    }

    /* positive responses echo (service | 0x40) then the identifier */
    if (payload_len < 1 || payload[0] != (uint8_t)(want[0] + 0x40))
    {
        return false;
    }

    if (n_want > 1 && payload_len > 1 && payload[1] != want[1])
    {
        return false;
    }

    return true;
}

/** Tokenize raw chip text into parsed data lines. @return line count;
 *  error lines set @p saw_error, a bare length line sets @p iso_total. */
static int collect_lines(const char *resp, ap_line_t *lines, int max,
                         bool *saw_error, long *iso_total)
{
    int n_lines = 0;
    const char *p = resp;

    while (*p != '\0' && n_lines < max)
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n')
        {
            eol++;
        }

        size_t len = (size_t)(eol - p);

        /* trim */
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '>'))
        {
            len--;
        }

        while (len > 0 && *p == ' ')
        {
            p++;
            len--;
        }

        if (len > 0)
        {
            char buf[AP_LINE_MAX];

            if (len >= sizeof(buf))
            {
                len = sizeof(buf) - 1;
            }

            memcpy(buf, p, len);
            buf[len] = '\0';

            if (line_is_error(buf, len))
            {
                *saw_error = true;
            }
            else if (!line_is_noise(buf, len))
            {
                ap_line_t parsed;

                if (parse_line(buf, len, &parsed))
                {
                    if (parsed.is_bare)
                    {
                        *iso_total = parsed.bare_value;
                    }
                    else
                    {
                        lines[n_lines++] = parsed;
                    }
                }
                /* unparseable line (command echo remnants) — skip */
            }
        }

        p = eol;

        while (*p == '\r' || *p == '\n')
        {
            p++;
        }
    }

    return n_lines;
}

/** ISO-TP reassembly over the frames of ONE responder id (PCI in byte
 *  0; non-ISO-TP lines taken raw). @return assembled byte count. */
static size_t assemble_header_frames(const ap_line_t *lines, int n_lines,
                                     uint32_t header, uint8_t *payload,
                                     size_t payload_max)
{
    size_t n = 0;
    long expect = -1;

    for (int i = 0; i < n_lines; i++)
    {
        if (!lines[i].has_header || lines[i].header != header ||
            lines[i].n == 0)
        {
            continue;
        }

        const uint8_t *b = lines[i].bytes;
        size_t bn = lines[i].n;
        uint8_t pci = b[0] >> 4;
        size_t start, count;

        if (pci == 0x0)
        {
            count = b[0] & 0x0F;
            start = 1;

            if (count > bn - 1)
            {
                count = bn - 1;
            }
        }
        else if (pci == 0x1)
        {
            if (bn < 2)
            {
                continue;
            }

            expect = ((long)(b[0] & 0x0F) << 8) | b[1];
            start = 2;
            count = bn - 2;
        }
        else if (pci == 0x2)
        {
            start = 1;
            count = bn - 1;
        }
        else
        {
            /* no PCI (11-bit non-ISO-TP data) — take the raw bytes */
            start = 0;
            count = bn;
        }

        for (size_t j = 0; j < count && n < payload_max; j++)
        {
            payload[n++] = b[start + j];
        }
    }

    if (expect >= 0 && (size_t)expect < n)
    {
        n = (size_t)expect;
    }

    return n;
}

/** Headers-off assembly: ISO-TP rows in row order (trimmed to the bare
 *  length line) or plain concatenation. @return byte count. */
static size_t assemble_headerless(const ap_line_t *lines, int n_lines,
                                  long iso_total, uint8_t *payload,
                                  size_t payload_max)
{
    size_t n = 0;

    if (lines[0].iso_index >= 0 || iso_total >= 0)
    {
        /* headers off, ISO-TP rows: concatenate in row order */
        for (int idx = 0; idx < n_lines; idx++)
        {
            for (int i = 0; i < n_lines; i++)
            {
                if (lines[i].iso_index == idx)
                {
                    for (size_t j = 0; j < lines[i].n && n < payload_max;
                         j++)
                    {
                        payload[n++] = lines[i].bytes[j];
                    }
                }
            }
        }

        if (iso_total >= 0 && (size_t)iso_total < n)
        {
            n = (size_t)iso_total;
        }
    }
    else
    {
        /* headers off, plain line(s): concatenate */
        for (int i = 0; i < n_lines; i++)
        {
            for (size_t j = 0; j < lines[i].n && n < payload_max; j++)
            {
                payload[n++] = lines[i].bytes[j];
            }
        }
    }

    return n;
}

esp_err_t ap_resp_to_payload(const char *resp, uint8_t *payload,
                             size_t payload_max, size_t *out_len)
{
    if (resp == NULL || payload == NULL || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;

    lines_lock();

    ap_line_t *lines = s_lines;
    bool saw_error = false;
    long iso_total = -1; /* headers-off multi-line length */
    int n_lines = collect_lines(resp, lines, AP_MAX_LINES, &saw_error,
                                &iso_total);

    if (saw_error || n_lines == 0)
    {
        lines_unlock();
        return saw_error ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }

    size_t n = 0;

    if (lines[0].has_header)
    {
        /* headers on: keep only the LOWEST responder id (legacy rule) */
        uint32_t lowest = UINT32_MAX;

        for (int i = 0; i < n_lines; i++)
        {
            if (lines[i].has_header && lines[i].header < lowest)
            {
                lowest = lines[i].header;
            }
        }

        n = assemble_header_frames(lines, n_lines, lowest, payload,
                                   payload_max);
    }
    else
    {
        n = assemble_headerless(lines, n_lines, iso_total, payload,
                                payload_max);
    }

    lines_unlock();

    if (n == 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    *out_len = n;
    return ESP_OK;
}

int ap_resp_to_payloads(const char *resp, ap_resp_ecu_t *out,
                        int max_ecus)
{
    if (resp == NULL || out == NULL || max_ecus <= 0)
    {
        return 0;
    }

    if (max_ecus > AP_RESP_ECUS_MAX)
    {
        max_ecus = AP_RESP_ECUS_MAX;
    }

    lines_lock();

    ap_line_t *lines = s_lines;
    bool saw_error = false;
    long iso_total = -1;
    int n_lines = collect_lines(resp, lines, AP_MAX_LINES, &saw_error,
                                &iso_total);

    if (saw_error || n_lines == 0)
    {
        lines_unlock();
        return saw_error ? -1 : 0;
    }

    if (!lines[0].has_header)
    {
        /* headers off — responders are indistinguishable; assemble the
           single-payload shapes from the lines already collected (NOT
           a nested ap_resp_to_payload call: its ~5.6 KB line table on
           top of ours is exactly the stack-overflow shape that
           corrupted the 2026-07-22 scans) */
        out[0].header = UINT32_MAX;
        out[0].len = assemble_headerless(lines, n_lines, iso_total,
                                         out[0].payload,
                                         sizeof(out[0].payload));
        lines_unlock();
        return (out[0].len > 0) ? 1 : 0;
    }

    /* distinct responder ids, ascending; over-cap keeps the lowest */
    uint32_t hdrs[AP_RESP_ECUS_MAX];
    int n_hdrs = 0;

    for (int i = 0; i < n_lines; i++)
    {
        if (!lines[i].has_header)
        {
            continue;
        }

        uint32_t h = lines[i].header;
        int at = 0;

        while (at < n_hdrs && hdrs[at] < h)
        {
            at++;
        }

        if (at < n_hdrs && hdrs[at] == h)
        {
            continue;
        }

        if (n_hdrs < max_ecus)
        {
            n_hdrs++;
        }
        else if (at >= max_ecus)
        {
            continue;           /* full and larger than everything kept  */
        }

        for (int k = n_hdrs - 1; k > at; k--)
        {
            hdrs[k] = hdrs[k - 1];
        }

        hdrs[at] = h;
    }

    int n_out = 0;

    for (int i = 0; i < n_hdrs; i++)
    {
        size_t len = assemble_header_frames(lines, n_lines, hdrs[i],
                                            out[n_out].payload,
                                            sizeof(out[n_out].payload));

        if (len > 0)
        {
            out[n_out].header = hdrs[i];
            out[n_out].len = len;
            n_out++;
        }
    }

    lines_unlock();
    return n_out;
}
