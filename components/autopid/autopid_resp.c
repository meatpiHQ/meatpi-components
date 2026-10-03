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
 *   18 DA F1 58 07 62 F4 00 .. headers on, 29-bit: the chip prints the id
 *                              as FOUR byte tokens (bench 2026-10-03).
 *                              Only the legislated answers 18DAF1xx are
 *                              taken as an id (take_id29)
 *
 * A `7F <service> 78` line (response pending: the ECU needs more time and
 * the chip keeps waiting) is not an answer and is dropped wherever it
 * stands; the lines after it are.
 *
 * The line tokenizer (one chip line -> bytes, header, row index) lives in
 * autopid_resp_lines.c since 2026-10-03 (700-line rule); this file
 * assembles its lines into payloads and holds the cross-talk guard.
 */
#include "autopid_private.h"
#include "autopid_resp_private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

bool ap_payload_matches_cmd(const char *cmd, const uint8_t *payload,
                            size_t payload_len)
{
    /* parse the leading hex byte pairs of the command ("010C", "22202A",
       "03"; a trailing response-count hint digit like "010C1" is ignored
       by pair alignment). Non-hex commands (AT/ST/VT) can't be checked. */
    uint8_t want[3];
    size_t n_want = 0;
    const char *p = cmd;

    while (n_want < 3)
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

    /* ReadDataByIdentifier echoes a 2-byte identifier: both bytes count,
       or `62 F4 0D` would pass as the answer to `22F40C` */
    if (want[0] == 0x22 && n_want > 2 && payload_len > 2 &&
        payload[2] != want[2])
    {
        return false;
    }

    return true;
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
    int n_lines = ap_resp_collect_lines(resp, lines, AP_MAX_LINES,
                                        &saw_error, &iso_total);

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
    int n_lines = ap_resp_collect_lines(resp, lines, AP_MAX_LINES,
                                        &saw_error, &iso_total);

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

