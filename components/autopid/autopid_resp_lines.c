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
 * @file autopid_resp_lines.c
 * @brief PURE: one line of chip text -> its bytes, CAN header and ISO-TP
 *        row index (host-tested). Split out of autopid_resp.c 2026-10-03
 *        (700-line rule); the shapes are listed there.
 */
#include "autopid_private.h"
#include "autopid_resp_private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

/**
 * With headers on the chip prints a 29-bit id as four byte tokens, and
 * until 2026-10-03 such a line was read as headers-off data: every 29-bit
 * answer failed the cross-talk guard (a DTC scan on a 29-bit car found no
 * ECU). Taken as an id only when it cannot be anything else: the bytes
 * `18 DA F1 xx` (an ECU answering the tester F1 on ISO 15765-4 normal fixed
 * addressing) followed by a PCI byte that fits the rest of the line. A
 * headers-off payload never starts with 0x18: no service answers with it,
 * and a raw first frame of 0x8DA bytes does not exist on this chip.
 */
static void take_id29(ap_line_t *line)
{
    const uint8_t *b = line->bytes;

    if (line->has_header || line->iso_index >= 0 || line->n < 6 ||
        b[0] != 0x18 || b[1] != 0xDA || b[2] != 0xF1)
    {
        return;
    }

    size_t rest = line->n - 5;          /* bytes after the PCI           */
    uint8_t pci = b[4];
    bool fits;

    switch (pci >> 4)
    {
        case 0x0:                       /* single frame: 1..7 bytes      */
            fits = (pci & 0x0F) >= 1 && (size_t)(pci & 0x0F) <= rest;
            break;

        case 0x1:                       /* first frame: length + data    */
            fits = rest >= 2;
            break;

        case 0x2:                       /* consecutive frame             */
            fits = true;
            break;

        default:
            fits = false;
            break;
    }

    if (!fits)
    {
        return;
    }

    line->header = 0x18DAF100u | b[3];
    line->has_header = true;
    line->n -= 4;
    memmove(line->bytes, &line->bytes[4], line->n);
}

/** `7F <service> 78`: the ECU asks for more time; the answer follows. */
static bool line_is_pending(const ap_line_t *line)
{
    const uint8_t *b = line->bytes;

    if (line->iso_index >= 0)
    {
        return false;
    }

    if (!line->has_header)
    {
        return line->n == 3 && b[0] == 0x7F && b[2] == 0x78;
    }

    /* headers on: a single frame of three bytes (padding may follow) */
    return line->n >= 4 && b[0] == 0x03 && b[1] == 0x7F && b[3] == 0x78;
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

/** Tokenize raw chip text into parsed data lines. @return line count;
 *  error lines set @p saw_error, a bare length line sets @p iso_total. */
int ap_resp_collect_lines(const char *resp, ap_line_t *lines, int max,
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
                    take_id29(&parsed);

                    if (parsed.is_bare)
                    {
                        *iso_total = parsed.bare_value;
                    }
                    else if (!line_is_pending(&parsed))
                    {
                        lines[n_lines++] = parsed;
                    }
                }
                /* unparseable line (command echo remnants): skip */
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
