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
 * @file obd_chip_kline.c
 * @brief The K-line session as the request engine keeps it (pure, no IDF
 *        deps, host-tested): ISO 9141-2 and ISO 14230-4, the chip's
 *        protocols 3, 4 and 5.
 *
 * A K-line bus is not just there like a CAN bus. It is STARTED, by a 5 baud
 * address or a fast init pattern, and the ECU ends the session by itself
 * when nothing is asked for five seconds (P3max). Measured on the MIC3624
 * V2.3.24 against ECU sim v2 (2026-11-12, the bench's K-line car):
 *
 * - the first request behind a protocol word starts the bus. The chip
 *   prints `BUS INIT: ` within 100 ms, a dot every 600 ms, then `OK` at
 *   2.6 s and the reply at 2.75 s (5 baud; a fast init prints `OK` at 0.5 s
 *   and the reply at 0.7 s). With no ECU there it prints `ERROR` at 2.9 s
 *   (1.0 s fast) and tries again at the next request;
 * - any byte sent to the chip meanwhile ends the start: the chip answers
 *   `STOPPED` and the word that interrupted it is lost. A request cut at
 *   1.5 s therefore never got an answer, and neither did the word behind
 *   it (GitHub issue #1045: the Test button, 3 of 3);
 * - the session lasts through 4.6 s without a request and is gone after
 *   5.0 s. The chip does not notice: it sends the next request, hears
 *   nothing and prints `NO DATA`, every time. Choosing the same protocol
 *   again changes nothing. `ATPC` (close the protocol) does: the request
 *   behind it starts the bus again;
 * - the chip keeps no session alive by itself. `ATSW` and `ATWM` are taken
 *   (`OK`) and nothing is sent on the bus between two requests.
 *
 * So the engine does two things for every requester (the poller, the Test
 * button, the detection, the trouble codes):
 *
 * - a request whose answer so far says `BUS INIT` is given the bus's start
 *   time instead of being cut at the caller's own (obd_parse_bus_starting);
 * - it remembers that a bus start ended in `OK` and when the bus last
 *   answered. A bus request that comes later than the session can last gets
 *   `ATPC` first, so it starts the bus instead of collecting `NO DATA`
 *   (obd_kline_stale).
 *
 * A `NO DATA` inside a live session is left alone: a K-line ECU answers a
 * request it does not support with silence.
 */
#include "obd_chip_private.h"

#include <ctype.h>
#include <string.h>

/** A word as the chip reads it: no blanks, upper case, up to the CR (or the
 *  ';' of a chain). @return its length. */
static size_t squeezed(const char *cmd, char *out, size_t cap)
{
    size_t n = 0;

    for (const char *p = cmd;
         p != NULL && *p != '\0' && *p != '\r' && *p != '\n' && *p != ';' &&
         n < cap - 1; p++)
    {
        if (*p != ' ' && *p != '\t')
        {
            out[n++] = (char)toupper((unsigned char)*p);
        }
    }

    out[n] = '\0';
    return n;
}

static bool contains(const char *text, size_t len, const char *what)
{
    size_t wlen = strlen(what);

    for (size_t i = 0; text != NULL && i + wlen <= len; i++)
    {
        if (memcmp(text + i, what, wlen) == 0)
        {
            return true;
        }
    }

    return false;
}

bool obd_parse_bus_starting(const char *text, size_t len)
{
    return contains(text, len, "BUS INIT");
}

/** The answer carries data from the bus: a line of hex digits and blanks
 *  and nothing else (`48 6B 10 41 0C ..`, `41 0C ..`, `7E8 06 41 ..`, the
 *  `0:` rows of a long reply), at least one byte of it. The chip's texts
 *  are not: `NO DATA`, `BUS INIT: ...ERROR`, `FB ERROR` and `FC RX TIMEOUT`
 *  (which begin with two hex digits), `?`, `OK`. */
static bool answered(const char *resp)
{
    size_t digits = 0;
    bool hex_only = true;

    for (const char *p = resp; p != NULL; p++)
    {
        if (*p == '\r' || *p == '\n' || *p == '\0')
        {
            if (hex_only && digits >= 2)
            {
                return true;
            }

            if (*p == '\0')
            {
                break;
            }

            digits = 0;
            hex_only = true;
        }
        else if (isxdigit((unsigned char)*p))
        {
            digits++;
        }
        else if (*p != ' ' && *p != ':')
        {
            hex_only = false;
        }
    }

    return false;
}

/** The protocol a switch word names: `ATTP3` -> '3', `ATSPA4` -> '4'; '?'
 *  for the STN and vendor presets (another bus, whichever). */
static char switch_proto(const char *c, size_t n)
{
    if (n >= 5 && c[0] == 'A' && c[1] == 'T' && (c[2] == 'S' || c[2] == 'T') &&
        c[3] == 'P')
    {
        return c[n - 1];
    }

    return '?';
}

void obd_kline_reset(obd_kline_t *k)
{
    k->up = false;
    k->proto = '\0';
    k->selected = '\0';
    k->last_us = 0;
}

void obd_kline_word(obd_kline_t *k, const char *cmd)
{
    char c[16];
    size_t n = squeezed(cmd, c, sizeof(c));

    if (strcmp(c, "ATPC") == 0 || strcmp(c, "ATZ") == 0 ||
        strcmp(c, "ATWS") == 0 || strcmp(c, "ATD") == 0)
    {
        /* the protocol is closed, or the chip is back on its stored one */
        k->up = false;

        if (c[2] != 'P')
        {
            k->selected = '\0';
        }

        return;
    }

    if (obd_parse_sets_protocol(cmd))
    {
        k->selected = switch_proto(c, n);

        bool kline = (k->selected >= '3' && k->selected <= '5');

        /* the same protocol chosen again leaves the session as it is (the
           prelude does that before every row type); another one ends it.
           A session that came up before any word of the engine named a
           protocol (the chip on its stored one) is taken for the K-line
           protocol named first: were it another, the chip starts the bus
           at the next request and says so */
        if (k->up && kline && k->proto == '\0')
        {
            k->proto = k->selected;
        }
        else if (k->up && k->selected != k->proto)
        {
            k->up = false;
        }
    }
}

void obd_kline_reply(obd_kline_t *k, const char *cmd, const char *resp,
                     int64_t now_us)
{
    if (resp == NULL || !obd_parse_is_bus_request(cmd))
    {
        return;
    }

    if (obd_parse_bus_starting(resp, strlen(resp)))
    {
        /* `BUS INIT: ...OK` (ATSI and ATFI print just that), or `...ERROR` */
        bool ok = contains(resp, strlen(resp), "OK") &&
                  !contains(resp, strlen(resp), "ERROR");

        k->up = ok;
        k->proto = k->selected;

        if (ok)
        {
            k->starts++;
            k->last_us = now_us;
        }
        else
        {
            k->failed++;
        }

        return;
    }

    if (k->up && answered(resp))
    {
        k->last_us = now_us;
    }
}

bool obd_kline_stale(const obd_kline_t *k, const char *cmd, int64_t now_us)
{
    char c[8];

    (void)squeezed(cmd, c, sizeof(c));

    /* the two words that start the bus themselves need no close first */
    if (!k->up || !obd_parse_is_bus_request(cmd) ||
        strncmp(c, "ATSI", 4) == 0 || strncmp(c, "ATFI", 4) == 0)
    {
        return false;
    }

    return now_us - k->last_us > (int64_t)OBD_KLINE_IDLE_MS * 1000;
}

uint32_t obd_kline_idle_ms(const obd_kline_t *k, int64_t now_us)
{
    if (!k->up)
    {
        return UINT32_MAX;
    }

    int64_t idle = (now_us - k->last_us) / 1000;

    return (idle < 0) ? 0 : (idle > UINT32_MAX - 1) ? UINT32_MAX - 1
                                                     : (uint32_t)idle;
}
