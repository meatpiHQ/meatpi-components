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
 * @file autopid_dtc_codec.c
 * @brief PURE DTC codec (TASK_dtc.md §4, host-tested): 2-byte code <->
 *        "P0420" text, mode 43/47/4A payload parse, 41-01 MIL/count
 *        parse, and the clear-condition evaluation (mode 04 is
 *        all-or-nothing, so "clear specific DTCs" is a condition on the
 *        whole present set).
 *
 * Inputs are ap_resp_to_payload() output (service echo byte first);
 * ap_payload_matches_cmd() has already guarded cross-talk upstream, but
 * every parser re-checks its service byte — a codec must not rely on
 * its caller's discipline.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <string.h>

/* ---- code <-> text ------------------------------------------------------------ */

void ap_dtc_format(uint8_t hi, uint8_t lo, char out[AP_DTC_CODE_LEN])
{
    static const char LETTER[4] = { 'P', 'C', 'B', 'U' };
    static const char HEX[] = "0123456789ABCDEF";

    out[0] = LETTER[(hi >> 6) & 0x3];
    out[1] = (char)('0' + ((hi >> 4) & 0x3));
    out[2] = HEX[hi & 0xF];
    out[3] = HEX[(lo >> 4) & 0xF];
    out[4] = HEX[lo & 0xF];
    out[5] = '\0';
}

bool ap_dtc_unformat(const char *code, uint8_t *hi, uint8_t *lo)
{
    if (code == NULL || hi == NULL || lo == NULL || strlen(code) != 5)
    {
        return false;
    }

    uint8_t letter;

    switch (toupper((unsigned char)code[0]))
    {
        case 'P': letter = 0; break;
        case 'C': letter = 1; break;
        case 'B': letter = 2; break;
        case 'U': letter = 3; break;
        default:  return false;
    }

    if (code[1] < '0' || code[1] > '3')
    {
        return false;
    }

    uint8_t nib[3];

    for (int i = 0; i < 3; i++)
    {
        char c = (char)toupper((unsigned char)code[2 + i]);

        if (c >= '0' && c <= '9')
        {
            nib[i] = (uint8_t)(c - '0');
        }
        else if (c >= 'A' && c <= 'F')
        {
            nib[i] = (uint8_t)(c - 'A' + 10);
        }
        else
        {
            return false;
        }
    }

    *hi = (uint8_t)((letter << 6) | ((code[1] - '0') << 4) | nib[0]);
    *lo = (uint8_t)((nib[1] << 4) | nib[2]);
    return true;
}

/* ---- mode 43/47/4A payload -> code list ---------------------------------------- */

int ap_dtc_parse_codes(const uint8_t *payload, size_t len,
                       uint8_t service_resp,
                       char out[][AP_DTC_CODE_LEN], size_t out_max)
{
    if (payload == NULL || len < 1 || payload[0] != service_resp)
    {
        return -1;
    }

    /* CAN format (ISO 15031-5): <svc> <count> <pairs...>. Tolerate a
     * missing count byte (some gateways strip it): if byte 1 makes the
     * remaining length odd, treat everything after the service byte as
     * pairs instead. */
    size_t off = 2;

    if (len >= 2 && ((len - 2) % 2) != 0 && ((len - 1) % 2) == 0)
    {
        off = 1;
    }

    if (len < off)
    {
        return 0;
    }

    size_t pairs = (len - off) / 2;
    int n = 0;

    for (size_t i = 0; i < pairs; i++)
    {
        uint8_t hi = payload[off + i * 2];
        uint8_t lo = payload[off + i * 2 + 1];

        if (hi == 0 && lo == 0)
        {
            continue;               /* padding */
        }

        if ((size_t)n >= out_max)
        {
            break;                  /* keep what fits */
        }

        ap_dtc_format(hi, lo, out[n]);
        n++;
    }

    return n;
}

uint8_t ap_dtc_merge_codes(char dst[][AP_DTC_CODE_LEN], uint8_t n_dst,
                           size_t dst_max,
                           const char src[][AP_DTC_CODE_LEN],
                           uint8_t n_src)
{
    for (uint8_t i = 0; i < n_src; i++)
    {
        bool dup = false;

        for (uint8_t j = 0; j < n_dst; j++)
        {
            if (strcmp(dst[j], src[i]) == 0)
            {
                dup = true;
                break;
            }
        }

        if (!dup && (size_t)n_dst < dst_max)
        {
            memcpy(dst[n_dst], src[i], AP_DTC_CODE_LEN);
            n_dst++;
        }
    }

    return n_dst;
}

/* ---- mode 41 01 -> MIL + count -------------------------------------------------- */

bool ap_dtc_parse_mil(const uint8_t *payload, size_t len, bool *mil,
                      uint8_t *count)
{
    /* 41 01 AA BB CC DD — A7 = MIL, A6..A0 = stored-DTC count */
    if (payload == NULL || len < 3 || payload[0] != 0x41 ||
        payload[1] != 0x01)
    {
        return false;
    }

    if (mil != NULL)
    {
        *mil = (payload[2] & 0x80) != 0;
    }

    if (count != NULL)
    {
        *count = (uint8_t)(payload[2] & 0x7F);
    }

    return true;
}

/* ---- clear-condition evaluation -------------------------------------------------- */

/** Case-insensitive membership of @p code in the CSV list. Whitespace
 *  around entries tolerated. Empty/NULL list = matches nothing. */
static bool csv_contains(const char *csv, const char *code)
{
    if (csv == NULL)
    {
        return false;
    }

    const char *p = csv;

    while (*p != '\0')
    {
        while (*p == ' ' || *p == ',')
        {
            p++;
        }

        const char *start = p;

        while (*p != '\0' && *p != ',' && *p != ' ')
        {
            p++;
        }

        size_t n = (size_t)(p - start);

        if (n == strlen(code))
        {
            size_t i = 0;

            while (i < n && toupper((unsigned char)start[i]) ==
                                toupper((unsigned char)code[i]))
            {
                i++;
            }

            if (i == n)
            {
                return true;
            }
        }
    }

    return false;
}

ap_dtc_clear_mode_t ap_dtc_clear_mode_parse(const char *mode,
                                            const char *codes)
{
    if (mode == NULL || mode[0] == '\0')
    {
        /* default: unconditional without a code list, if_any with one */
        return (codes == NULL || codes[0] == '\0') ? AP_DTC_CLEAR_ALWAYS
                                                   : AP_DTC_CLEAR_IF_ANY;
    }

    if (strcmp(mode, "always") == 0)
    {
        return AP_DTC_CLEAR_ALWAYS;
    }

    if (strcmp(mode, "if_any") == 0)
    {
        return AP_DTC_CLEAR_IF_ANY;
    }

    if (strcmp(mode, "if_only") == 0)
    {
        return AP_DTC_CLEAR_IF_ONLY;
    }

    return AP_DTC_CLEAR_INVALID;
}

bool ap_dtc_clear_allowed(const char present[][AP_DTC_CODE_LEN],
                          size_t n_present, const char *codes,
                          ap_dtc_clear_mode_t mode)
{
    switch (mode)
    {
        case AP_DTC_CLEAR_ALWAYS:
            return true;

        case AP_DTC_CLEAR_IF_ANY:
            for (size_t i = 0; i < n_present; i++)
            {
                if (csv_contains(codes, present[i]))
                {
                    return true;
                }
            }

            return false;

        case AP_DTC_CLEAR_IF_ONLY:
            /* nothing present = nothing to protect = nothing to clear:
             * refuse so the caller doesn't burn a mode 04 (and the
             * readiness monitors) for no effect */
            if (n_present == 0)
            {
                return false;
            }

            for (size_t i = 0; i < n_present; i++)
            {
                if (!csv_contains(codes, present[i]))
                {
                    return false;   /* an unlisted code would be wiped */
                }
            }

            return true;

        default:
            return false;
    }
}

/* ---- freeze frame (mode 02) — TASK_dtc §14 ---------------------------------- */

bool ap_frz_dtc(const uint8_t *payload, size_t len,
                char out[AP_DTC_CODE_LEN])
{
    /* [0x42, 0x02, frame, hi, lo] — echo-validated (cross-talk guard) */
    if (payload == NULL || len < 5 || payload[0] != 0x42 ||
        payload[1] != 0x02)
    {
        return false;
    }

    if (payload[3] == 0x00 && payload[4] == 0x00)
    {
        return false;   /* zeros = no frame stored (some ECUs do this) */
    }

    ap_dtc_format(payload[3], payload[4], out);
    return true;
}

bool ap_frz_bitmap(const uint8_t *payload, size_t len, uint8_t pid,
                   uint32_t *bitmap)
{
    if (payload == NULL || bitmap == NULL || len < 7 ||
        payload[0] != 0x42 || payload[1] != pid)
    {
        return false;
    }

    *bitmap = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[4] << 16) |
              ((uint32_t)payload[5] << 8) | (uint32_t)payload[6];
    return true;
}
