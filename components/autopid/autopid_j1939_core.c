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
 * @file autopid_j1939_core.c
 * @brief PURE (host-tested, host_test/main/test_j1939_rows.c): the `PGN:`
 *        row grammar, the scheduling class of a row, and the expression
 *        that decodes one SPN of the built-in table. The grammar itself is
 *        described in autopid_j1939.h. No IDF dependencies.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PGN_PREFIX     "PGN:"
#define PGN_PREFIX_LEN 4
#define PGN_HEX_MAX    5        /* 0x3FFFF                                  */
#define PDU1_PF_LIMIT  240      /* PF below it: the PS byte is a destination */

/* ---- the grammar ----------------------------------------------------------- */

/** Up to @p max hex digits at @p s into @p out. @return digits read. */
static int read_hex(const char *s, int max, uint32_t *out)
{
    int n = 0;

    *out = 0;

    while (n < max && isxdigit((unsigned char)s[n]))
    {
        char c = (char)toupper((unsigned char)s[n]);

        *out = (*out << 4) | (uint32_t)((c <= '9') ? c - '0' : c - 'A' + 10);
        n++;
    }

    return n;
}

esp_err_t ap_pgn_cmd_parse(const char *cmd, uint32_t *pgn, int16_t *sa,
                           bool *request)
{
    if (cmd == NULL || pgn == NULL || sa == NULL || request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = cmd;

    while (*p == ' ')
    {
        p++;
    }

    if (strncasecmp(p, PGN_PREFIX, PGN_PREFIX_LEN) != 0)
    {
        return ESP_ERR_NOT_FOUND;       /* a chip request */
    }

    p += PGN_PREFIX_LEN;

    uint32_t value = 0;
    int n = read_hex(p, PGN_HEX_MAX, &value);

    if (n == 0 || isxdigit((unsigned char)p[n]) || value > AP_PGN_MAX)
    {
        return ESP_ERR_INVALID_ARG;     /* no number, or too long          */
    }

    if (((value >> 8) & 0xFF) < PDU1_PF_LIMIT && (value & 0xFF) != 0)
    {
        return ESP_ERR_INVALID_ARG;     /* a PDU1 group carries no address */
    }

    p += n;

    int source = J1939_ADDR_ANY;

    if (*p == '@')
    {
        p++;

        char *end = NULL;
        long v = strtol(p, &end, 0); /* decimal or 0x.. */

        if (end == p || v < 0 || v > AP_J1939_SA_MAX)
        {
            return ESP_ERR_INVALID_ARG;
        }

        source = (int)v;
        p = end;
    }

    bool asked = false;

    if (*p == '?')
    {
        asked = true;
        p++;
    }

    while (*p == ' ')
    {
        p++;
    }

    if (*p != '\0')
    {
        return ESP_ERR_INVALID_ARG;     /* trailing garbage */
    }

    *pgn = value;
    *sa = (int16_t)source;
    *request = asked;
    return ESP_OK;
}

size_t ap_pgn_cmd_format(uint32_t pgn, int sa, bool request, char *out,
                         size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return 0;
    }

    int n;

    if (sa >= 0)
    {
        n = snprintf(out, cap, "%s%lX@%d%s", PGN_PREFIX, (unsigned long)pgn,
                     sa, request ? "?" : "");
    }
    else
    {
        n = snprintf(out, cap, "%s%lX%s", PGN_PREFIX, (unsigned long)pgn,
                     request ? "?" : "");
    }

    if (n < 0 || (size_t)n >= cap)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)n;
}

/* ---- classes --------------------------------------------------------------- */

ap_row_class_t ap_row_class(const ap_pid_t *pid)
{
    if (pid == NULL || !pid->j1939)
    {
        return AP_CLASS_CHIP;
    }

    return pid->j1939_request ? AP_CLASS_BUS_TX : AP_CLASS_PASSIVE;
}

const char *ap_row_class_name(ap_row_class_t c)
{
    switch (c)
    {
    case AP_CLASS_PASSIVE: return "passive";
    case AP_CLASS_BUS_TX:  return "bus_tx";
    default:               return "chip";
    }
}

/* ---- the built-in table's expressions -------------------------------------- */

/** A number without trailing zeros ("0.125", "1", "0.4", "0.00390625":
 *  the grammar has no exponent, and 1/256 needs every digit). */
static void num_text(double v, char *out, size_t cap)
{
    snprintf(out, cap, "%.10f", v);

    char *dot = strchr(out, '.');

    if (dot != NULL)
    {
        char *e = out + strlen(out) - 1;

        while (e > dot && *e == '0')
        {
            *e-- = '\0';
        }

        if (e == dot)
        {
            *e = '\0';
        }
    }
}

size_t ap_j1939_std_expression(const j1939_spn_t *spn, char *out, size_t cap)
{
    if (spn == NULL || out == NULL || cap == 0 || spn->bits == 0 ||
        spn->bits > 32)
    {
        return 0;
    }

    char raw[64];
    size_t n = 0;

    if (spn->bit == 0 && (spn->bits % 8) == 0)
    {
        /* whole bytes, least significant first: B3+B4*256+B5*65536.. */
        unsigned bytes = spn->bits / 8;

        for (unsigned i = 0; i < bytes && n < sizeof(raw); i++)
        {
            int w;

            if (i == 0)
            {
                w = snprintf(raw + n, sizeof(raw) - n, "B%u",
                             (unsigned)spn->byte);
            }
            else
            {
                w = snprintf(raw + n, sizeof(raw) - n, "+B%u*%lu",
                             (unsigned)spn->byte + i,
                             (unsigned long)(1ul << (8 * i)));
            }

            n += (w > 0) ? (size_t)w : 0;
        }
    }
    else if (spn->bit + spn->bits <= 8)
    {
        /* a field inside one byte */
        snprintf(raw, sizeof(raw), "(B%u>>%u)&%u", (unsigned)spn->byte,
                 (unsigned)spn->bit, (unsigned)((1u << spn->bits) - 1));
    }
    else
    {
        return 0;   /* a field across bytes: not in the table */
    }

    char scale[24], offset[24];
    int w;

    num_text(spn->scale, scale, sizeof(scale));
    num_text(spn->offset < 0 ? -spn->offset : spn->offset, offset,
             sizeof(offset));

    bool unit_scale = (spn->scale == 1.0);
    bool no_offset = (spn->offset == 0.0);
    bool composite = (strchr(raw, '+') != NULL || raw[0] == '(');

    if (unit_scale && no_offset)
    {
        w = snprintf(out, cap, "%s", raw);
    }
    else if (unit_scale)
    {
        w = snprintf(out, cap, "%s%c%s", raw, (spn->offset < 0) ? '-' : '+',
                     offset);
    }
    else if (no_offset)
    {
        w = composite ? snprintf(out, cap, "(%s)*%s", raw, scale)
                      : snprintf(out, cap, "%s*%s", raw, scale);
    }
    else
    {
        w = composite ? snprintf(out, cap, "(%s)*%s%c%s", raw, scale,
                                 (spn->offset < 0) ? '-' : '+', offset)
                      : snprintf(out, cap, "%s*%s%c%s", raw, scale,
                                 (spn->offset < 0) ? '-' : '+', offset);
    }

    if (w < 0 || (size_t)w >= cap)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)w;
}
