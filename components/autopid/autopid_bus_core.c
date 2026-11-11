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
 * @file autopid_bus_core.c
 * @brief PURE half of a row's bus (autopid_bus.h, host-tested): the config
 *        words, the chip's slot per bus and the chain that takes the chip
 *        there, and which chains of the tables may ride on such a row.
 */
#include "autopid_bus.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "autopid_private.h"

/* The slot definitions (bench 2026-11-11, testing/chip_can_slot_bench.md):
 * `VTSET_CAN <slot>,<option>,<divisor>,<transceiver>[,<mode>]`. Option 81 =
 * 11-bit ids with ISO 15765-4 formatting, 91 = the same with the bit rate
 * times 8/7; the divisor is 500 kbit/s over it (04 = 125, 0F = 33.3,
 * 01 = 500); the last argument of the single-wire slot is its transceiver's
 * NORMAL mode (3). The high-voltage wake mode wakes every module on a car's
 * single-wire bus: no automatic path may send it, so there is no way to ask
 * for it here. The print format first (spaces, headers on, the timeout): a
 * row of another bus can be the first thing the chip is asked after a
 * reset. */
#define BUS_FORMAT  "ATS1;ATH1;ATST96;"
#define BUS_TAIL    ";ATSH7DF;ATCRA"
#define BUS_OPTION  0x81u
#define BUS_X87     0x10u
#define BUS_DIV_MAX 64

typedef struct
{
    const char *word;
    const char *label;
    char        slot;
    const char *transceiver;  /* with the single-wire mode behind it */
    uint8_t     divisor;      /* the bus's own rate                  */
} bus_def_t;

static const bus_def_t BUS[AP_BUS_COUNT] =
{
    [AP_BUS_OBD] = { "obd", "the car's OBD protocol", '\0', NULL, 0 },
    [AP_BUS_MS]  = { "ms", "MS CAN", 'B', "MS_CAN", 4 },
    [AP_BUS_SW]  = { "sw", "single-wire CAN", 'F', "SW_CAN,3", 15 },
    [AP_BUS_CH]  = { "ch", "CH CAN", 'C', "CH_CAN", 1 },
};

static const bus_def_t *def_of(uint8_t bus)
{
    return &BUS[(bus < AP_BUS_COUNT) ? bus : AP_BUS_OBD];
}

bool ap_bus_parse(const char *word, ap_row_bus_t *out)
{
    if (word == NULL || word[0] == '\0')
    {
        *out = AP_BUS_OBD;
        return true;
    }

    for (int b = 0; b < AP_BUS_COUNT; b++)
    {
        if (strcmp(word, BUS[b].word) == 0)
        {
            *out = (ap_row_bus_t)b;
            return true;
        }
    }

    return false;
}

const char *ap_bus_word(uint8_t bus)
{
    return def_of(bus)->word;
}

const char *ap_bus_label(uint8_t bus)
{
    return def_of(bus)->label;
}

char ap_bus_slot(uint8_t bus)
{
    return def_of(bus)->slot;
}

/** bit/s of a divisor, with or without the 8/7 multiplier. */
static uint32_t rate_bps(unsigned divisor, bool x87)
{
    uint32_t base = x87 ? 4000000u / 7u : 500000u; /* 500 kbit/s * 8 / 7 */

    return (base + divisor / 2) / divisor;
}

bool ap_bus_rate_parse(uint8_t bus, double kbps, uint8_t *code)
{
    *code = 0;

    if (kbps <= 0)
    {
        return true; /* no speed named: the bus's own */
    }

    if (bus == AP_BUS_OBD || bus >= AP_BUS_COUNT || kbps > 1000)
    {
        return false; /* the car's protocol has its speed in its number */
    }

    double want = kbps * 1000.0;

    /* the plain divisors first: 83.3 is 500 / 6, not 571.4 / 7 */
    for (int x87 = 0; x87 <= 1; x87++)
    {
        for (unsigned d = 1; d <= BUS_DIV_MAX; d++)
        {
            double have = (double)rate_bps(d, x87 != 0);
            double off = (have > want) ? have - want : want - have;

            if (off * 100.0 <= want) /* within 1 % */
            {
                bool own = (x87 == 0 && d == def_of(bus)->divisor);

                *code = own ? 0
                            : (uint8_t)(d | (x87 ? AP_BUS_RATE_X87 : 0u));
                return true;
            }
        }
    }

    return false;
}

uint32_t ap_bus_rate_bps(uint8_t bus, uint8_t code)
{
    if (bus == AP_BUS_OBD || bus >= AP_BUS_COUNT)
    {
        return 0;
    }

    unsigned d = code & 0x7Fu;

    return (code == 0 || d == 0 || d > BUS_DIV_MAX)
               ? rate_bps(def_of(bus)->divisor, false)
               : rate_bps(d, (code & AP_BUS_RATE_X87) != 0);
}

size_t ap_bus_chain(uint16_t key, char *out, size_t cap)
{
    uint8_t bus = AP_BUS_OF(key);
    uint8_t code = AP_BUS_RATE_OF(key);
    const bus_def_t *b = def_of(bus);

    if (out == NULL || cap == 0)
    {
        return 0;
    }

    out[0] = '\0';

    if (bus == AP_BUS_OBD || bus >= AP_BUS_COUNT)
    {
        return 0;
    }

    unsigned d = code & 0x7Fu;
    bool own = (code == 0 || d == 0 || d > BUS_DIV_MAX);
    unsigned option = BUS_OPTION |
                      ((!own && (code & AP_BUS_RATE_X87)) ? BUS_X87 : 0u);
    int n = snprintf(out, cap,
                     BUS_FORMAT "VTSET_CAN %c,%02X,%02X,%s;ATTP%c" BUS_TAIL,
                     b->slot, option, own ? b->divisor : d, b->transceiver,
                     b->slot);

    if (n < 0 || (size_t)n >= cap)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)n;
}

/** One command of a chain as the chip reads it: spaces dropped, upper case. */
static size_t squeeze(const char *cmd, size_t len, char *out, size_t cap)
{
    size_t n = 0;

    for (size_t i = 0; i < len && n + 1 < cap; i++)
    {
        if (cmd[i] != ' ' && cmd[i] != '\t')
        {
            out[n++] = (char)toupper((unsigned char)cmd[i]);
        }
    }

    out[n] = '\0';
    return n;
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/** One to three hex digits and the end: `53`, `2C3` (not `BR250000`). */
static bool preset_number(const char *s)
{
    size_t n = 0;

    while (isxdigit((unsigned char)s[n]))
    {
        n++;
    }

    return n >= 1 && n <= 3 && s[n] == '\0';
}

/** Does one command pick a protocol, define a slot, link a transceiver or
 *  reset the chip? */
static bool cmd_moves_bus(const char *cmd, size_t len)
{
    char s[40];
    size_t n = squeeze(cmd, len, s, sizeof(s));

    if (n == 0)
    {
        return false;
    }

    if (ap_guard_cmd_proto(cmd, len) != '\0' || ap_guard_cmd_resets(cmd, len))
    {
        return true; /* ATSP / ATTP in every form; ATZ, ATD, ATWS */
    }

    /* ATTPD..F, the STN and vendor presets, the slot and link words, the
       ELM form of a slot's options (`ATPB xx yy`). STPBR / VTPBR / STPBRR
       (a bit rate behind the slot, RAM) and STPTO and friends are not
       protocol choices: `STP` and `VTP` count only as a preset number,
       hex digits and nothing else behind them */
    return starts(s, "ATSP") || starts(s, "ATTP") ||
           ((starts(s, "STP") || starts(s, "VTP")) && preset_number(s + 3)) ||
           starts(s, "VTSET_CAN") || starts(s, "VTCFG_") ||
           starts(s, "ATLNK") || starts(s, "ATPB");
}

bool ap_bus_chain_allowed(const char *chain)
{
    if (chain == NULL)
    {
        return true;
    }

    const char *start = chain;

    for (const char *p = chain;; p++)
    {
        if (*p == ';' || *p == '\0')
        {
            if (cmd_moves_bus(start, (size_t)(p - start)))
            {
                return false;
            }

            if (*p == '\0')
            {
                break;
            }

            start = p + 1;
        }
    }

    return true;
}

bool ap_bus_err_note(ap_bus_err_t *e, bool can_error, int64_t now_us)
{
    if (!can_error)
    {
        e->streak = 0;   /* a frame got through at this speed */
        e->park_us = 0;
        return false;
    }

    if (++e->streak < AP_BUS_ERR_STREAK)
    {
        return false;
    }

    e->streak = 0;
    e->park_us = (e->park_us == 0)                         ? AP_BUS_PARK_MIN_US
                 : (e->park_us * 2 > AP_BUS_PARK_MAX_US)   ? AP_BUS_PARK_MAX_US
                                                           : e->park_us * 2;
    e->until_us = now_us + e->park_us;
    e->parks++;
    return true;
}

bool ap_bus_err_parked(const ap_bus_err_t *e, int64_t now_us)
{
    return e->park_us != 0 && now_us < e->until_us;
}
