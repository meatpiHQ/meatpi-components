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
