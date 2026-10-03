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

/** @file uds_dtc_codec.c — PURE (no I/O); see uds_dtc.h / TASK_dtc §12. */
#include "uds_dtc.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

size_t uds_dtc_req_count(uint8_t status_mask, uint8_t out[3])
{
    out[0] = 0x19;
    out[1] = 0x01;
    out[2] = status_mask;
    return 3;
}

size_t uds_dtc_req_by_status(uint8_t status_mask, uint8_t out[3])
{
    out[0] = 0x19;
    out[1] = 0x02;
    out[2] = status_mask;
    return 3;
}

size_t uds_dtc_req_supported(uint8_t out[2])
{
    out[0] = 0x19;
    out[1] = 0x0A;
    return 2;
}

size_t uds_dtc_req_clear(const uint8_t group[3], uint8_t out[4])
{
    out[0] = 0x14;

    if (group == NULL)
    {
        out[1] = 0xFF;
        out[2] = 0xFF;
        out[3] = 0xFF;
    }
    else
    {
        out[1] = group[0];
        out[2] = group[1];
        out[3] = group[2];
    }

    return 4;
}

bool uds_dtc_parse_count(const uint8_t *resp, size_t len,
                         uint8_t *avail_mask, uint16_t *count)
{
    /* 59 01 <availMask> <formatIdentifier> <count hi> <count lo> */
    if (resp == NULL || len < 6 || resp[0] != 0x59 || resp[1] != 0x01)
    {
        return false;
    }

    if (avail_mask != NULL)
    {
        *avail_mask = resp[2];
    }

    if (count != NULL)
    {
        *count = (uint16_t)((resp[4] << 8) | resp[5]);
    }

    return true;
}

int uds_dtc_parse_list(const uint8_t *resp, size_t len,
                       uint8_t *avail_mask, uds_dtc_t *out, size_t max)
{
    if (resp == NULL || len < 3 || resp[0] != 0x59 ||
        (resp[1] != 0x02 && resp[1] != 0x0A))
    {
        return -1;
    }

    if (avail_mask != NULL)
    {
        *avail_mask = resp[2];
    }

    int n = 0;
    size_t off = 3;

    while (off + 4 <= len && (size_t)n < max)
    {
        out[n].hi = resp[off];
        out[n].mid = resp[off + 1];
        out[n].ftb = resp[off + 2];
        out[n].status = resp[off + 3];
        off += 4;
        n++;
    }

    return n;
}

bool uds_dtc_clear_ok(const uint8_t *resp, size_t len)
{
    return resp != NULL && len >= 1 && resp[0] == 0x54;
}

void uds_dtc_format(uint8_t hi, uint8_t mid, uint8_t ftb, char *out)
{
    static const char LETTER[4] = { 'P', 'C', 'B', 'U' };

    /* same 2-byte encoding as OBD (bits 15-14 letter, 13-12 first
       digit, then three hex nibbles) + the UDS failure-type suffix */
    int n = sprintf(out, "%c%u%X%02X", LETTER[(hi >> 6) & 0x3],
                    (hi >> 4) & 0x3, hi & 0x0F, mid);

    if (ftb != 0)
    {
        sprintf(out + n, "-%02X", ftb);
    }
}

/* ---- WWH-OBD (ISO 27145-3) / SAE J1979-2 ------------------------------------ */

size_t uds_wwh_req_by_mask(uint8_t group, uint8_t status_mask,
                           uint8_t severity_mask, uint8_t out[5])
{
    out[0] = 0x19;
    out[1] = 0x42;
    out[2] = group;
    out[3] = status_mask;
    out[4] = severity_mask;
    return 5;
}

size_t uds_wwh_req_permanent(uint8_t group, uint8_t out[3])
{
    out[0] = 0x19;
    out[1] = 0x55;
    out[2] = group;
    return 3;
}

size_t uds_wwh_req_clear(uint8_t group, uint8_t out[4])
{
    out[0] = 0x14;
    out[1] = 0xFF;
    out[2] = 0xFF;
    out[3] = group;
    return 4;
}

/** The records behind a header of @p hdr bytes; @p with_severity = five
 *  bytes each (severity first), else four. */
static int wwh_records(const uint8_t *resp, size_t len, size_t hdr,
                       bool with_severity, uds_wwh_dtc_t *out, size_t max)
{
    size_t rec = with_severity ? 5 : 4;
    size_t off = hdr;
    int n = 0;

    while (off + rec <= len && out != NULL && (size_t)n < max)
    {
        const uint8_t *p = &resp[off];

        if (with_severity)
        {
            out[n].severity = p[0];
            p++;
        }
        else
        {
            out[n].severity = 0;
        }

        out[n].dtc[0] = p[0];
        out[n].dtc[1] = p[1];
        out[n].dtc[2] = p[2];
        out[n].status = p[3];
        off += rec;
        n++;
    }

    return n;
}

int uds_wwh_parse_by_mask(const uint8_t *resp, size_t len, uint8_t group,
                          uint8_t *format, uds_wwh_dtc_t *out, size_t max)
{
    /* 59 42 <group> <statusAvail> <severityAvail> <format> records.. */
    if (resp == NULL || len < 6 || resp[0] != 0x59 || resp[1] != 0x42 ||
        resp[2] != group)
    {
        return -1;
    }

    if (format != NULL)
    {
        *format = resp[5];
    }

    return wwh_records(resp, len, 6, true, out, max);
}

int uds_wwh_parse_permanent(const uint8_t *resp, size_t len, uint8_t group,
                            uint8_t *format, uds_wwh_dtc_t *out, size_t max)
{
    /* 59 55 <group> <statusAvail> <format> records.. */
    if (resp == NULL || len < 5 || resp[0] != 0x59 || resp[1] != 0x55 ||
        resp[2] != group)
    {
        return -1;
    }

    if (format != NULL)
    {
        *format = resp[4];
    }

    return wwh_records(resp, len, 5, false, out, max);
}

void uds_wwh_dtc_text(uint8_t format, const uds_wwh_dtc_t *dtc,
                      char out[UDS_DTC_TEXT_LEN])
{
    if (format == UDS_DTC_FORMAT_J1939)
    {
        /* SAE J1939-73: SPN low byte, SPN middle byte, then the three
           high SPN bits above the five FMI bits */
        unsigned long spn = (unsigned long)dtc->dtc[0] |
                            ((unsigned long)dtc->dtc[1] << 8) |
                            ((unsigned long)(dtc->dtc[2] >> 5) << 16);

        snprintf(out, UDS_DTC_TEXT_LEN, "SPN%lu-%u", spn,
                 (unsigned)(dtc->dtc[2] & 0x1F));
        return;
    }

    uds_dtc_format(dtc->dtc[0], dtc->dtc[1], dtc->dtc[2], out);
}

bool uds_dtc_unformat(const char *code, uint8_t *hi, uint8_t *mid,
                      uint8_t *ftb)
{
    if (code == NULL || strlen(code) < 5)
    {
        return false;
    }

    int letter;

    switch (toupper((unsigned char)code[0]))
    {
        case 'P': letter = 0; break;
        case 'C': letter = 1; break;
        case 'B': letter = 2; break;
        case 'U': letter = 3; break;
        default:  return false;
    }

    if (code[1] < '0' || code[1] > '3' ||
        !isxdigit((unsigned char)code[2]) ||
        !isxdigit((unsigned char)code[3]) ||
        !isxdigit((unsigned char)code[4]))
    {
        return false;
    }

    unsigned d1 = (unsigned)(code[1] - '0');
    unsigned h2, h3, h4;

    sscanf(code + 2, "%1x", &h2);
    sscanf(code + 3, "%1x", &h3);
    sscanf(code + 4, "%1x", &h4);

    if (hi != NULL)
    {
        *hi = (uint8_t)((letter << 6) | (d1 << 4) | h2);
    }

    if (mid != NULL)
    {
        *mid = (uint8_t)((h3 << 4) | h4);
    }

    uint8_t f = 0;

    if (code[5] == '-')
    {
        unsigned v;

        if (!isxdigit((unsigned char)code[6]) ||
            !isxdigit((unsigned char)code[7]) || code[8] != '\0')
        {
            return false;
        }

        sscanf(code + 6, "%2x", &v);
        f = (uint8_t)v;
    }
    else if (code[5] != '\0')
    {
        return false;
    }

    if (ftb != NULL)
    {
        *ftb = f;
    }

    return true;
}
