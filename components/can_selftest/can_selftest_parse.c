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
 * @file can_selftest_parse.c
 * @brief The pure half (host-tested): the chip's answer to the engine speed
 *        request classified, the speed decoded, the verdict line.
 */
#include "can_selftest.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/** Parse up to @p max hex tokens of one line into @p out; returns the count.
 *  Tokens are 1 to 8 hex digits separated by spaces; anything else stops. */
static size_t hex_tokens(const char *line, uint32_t *out, size_t max)
{
    size_t n = 0;
    const char *p = line;

    while (n < max)
    {
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }

        if (!isxdigit((unsigned char)*p))
        {
            break;
        }

        char *end;
        unsigned long v = strtoul(p, &end, 16);

        if (end - p > 8 || (*end != '\0' && *end != ' ' && *end != '\t' &&
                            *end != '\r' && *end != '\n'))
        {
            break;
        }

        out[n++] = (uint32_t)v;
        p = end;
    }

    return n;
}

can_selftest_reply_t can_selftest_parse_reply(const char *resp,
                                              uint32_t want_id, int *rpm_out)
{
    can_selftest_reply_t verdict = CAN_SELFTEST_REPLY_NONE;
    bool other = false;

    if (rpm_out != NULL)
    {
        *rpm_out = -1;
    }

    if (resp == NULL)
    {
        return CAN_SELFTEST_REPLY_NONE;
    }

    const char *p = resp;

    while (*p != '\0')
    {
        /* one line at a time */
        const char *e = p;

        while (*e != '\0' && *e != '\r' && *e != '\n')
        {
            e++;
        }

        size_t len = (size_t)(e - p);
        char line[96];

        if (len >= sizeof(line))
        {
            len = sizeof(line) - 1;
        }

        memcpy(line, p, len);
        line[len] = '\0';

        /* the error texts the MIC prints (the ELM327 vocabulary) */
        if (strstr(line, "NO DATA") != NULL)
        {
            if (verdict == CAN_SELFTEST_REPLY_NONE)
            {
                verdict = CAN_SELFTEST_REPLY_NODATA;
            }
        }
        else if (strstr(line, "CAN ERROR") != NULL ||
                 strstr(line, "BUS ERROR") != NULL ||
                 strstr(line, "BUS BUSY") != NULL)
        {
            verdict = CAN_SELFTEST_REPLY_CANERROR;
        }
        else if (strstr(line, "ERROR") != NULL ||
                 strstr(line, "UNABLE") != NULL || strchr(line, '?') != NULL)
        {
            if (verdict == CAN_SELFTEST_REPLY_NONE)
            {
                verdict = CAN_SELFTEST_REPLY_ERROR;
            }
        }
        else
        {
            uint32_t tok[12];
            size_t n = hex_tokens(line, tok, 12);

            /* headers on: id, then the frame's bytes (PCI 04, 41, 0C, A, B) */
            if (n >= 5 && tok[0] == want_id && tok[2] == 0x41 && tok[3] == 0x0C)
            {
                if (rpm_out != NULL && n >= 6)
                {
                    *rpm_out = (int)((tok[4] * 256u + tok[5]) / 4u);
                }

                return CAN_SELFTEST_REPLY_OURS;
            }

            if (n >= 3 && tok[0] != want_id)
            {
                other = true;
            }
        }

        p = e;

        while (*p == '\r' || *p == '\n')
        {
            p++;
        }
    }

    if (verdict == CAN_SELFTEST_REPLY_NONE && other)
    {
        return CAN_SELFTEST_REPLY_OTHER;
    }

    return verdict;
}

const char *can_selftest_verdict(bool esp_heard, can_selftest_reply_t reply,
                                 bool *pass)
{
    bool ok = esp_heard && reply == CAN_SELFTEST_REPLY_OURS;

    if (pass != NULL)
    {
        *pass = ok;
    }

    if (ok)
    {
        return "PASS: both transceivers see each other";
    }

    if (reply == CAN_SELFTEST_REPLY_CANERROR)
    {
        return "the chip could not send: nothing on the bus acknowledged it, "
               "the shared wires or the termination";
    }

    if (!esp_heard)
    {
        if (reply == CAN_SELFTEST_REPLY_OTHER)
        {
            return "the ESP32 heard nothing although other ECUs answered the "
                   "chip: the ESP32's transceiver or its standby pin";
        }

        return "the chip asked but the ESP32 heard nothing: the chip's CAN "
               "side or its protocol, or the ESP32's transceiver";
    }

    if (reply == CAN_SELFTEST_REPLY_NODATA || reply == CAN_SELFTEST_REPLY_OTHER)
    {
        return "the ESP32 answered but the chip never printed it: the ESP32's "
               "transmit path or its standby pin";
    }

    if (reply == CAN_SELFTEST_REPLY_NONE)
    {
        return "the chip printed nothing in time";
    }

    return "the chip reported an error: see the exchange";
}
