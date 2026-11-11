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
 * @file autopid_bus.h
 * @brief The bus of a row (2026-11-11): the CAN buses of the OBD connector
 *        beside the car's OBD protocol.
 *
 * A row asks on the car's OBD protocol (`obd`, the default: whatever the
 * baseline pins, on pins 6 and 14) or on one of the buses only the OBD chip
 * reaches: MS CAN (`ms`, pins 3 and 11, 125 kbit/s), single-wire CAN (`sw`,
 * pin 1, 33.3 kbit/s) or CH CAN (`ch`, pins 12 and 13, 500 kbit/s), 11-bit
 * ids with ISO 15765-4 formatting. A car whose bus runs at another speed
 * names it (`bus_kbps`): any rate the chip can make, 500 kbit/s over 1 to
 * 64, or that times 8/7 (250, 125, 100, 95.2, 83.3, 50, 33.3 ...).
 *
 * NOTHING of this writes the chip's memory. The chip stores a protocol
 * preset (`STP nn`, `VTP 2Cn`) at every selection and a link word
 * (`ATLNK..`) as well (bench 2026-11-11, both kept through a power-off), so
 * neither is ever sent. A bus is reached through one of the chip's AT user
 * protocols: `VTSET_CAN <slot>,<option>,<divisor>,<transceiver>` defines the
 * slot's id length, bit rate and transceiver in the chip's RAM (gone with a
 * reset), and `ATTP<slot>` chooses it (RAM as well, behind the driver's
 * `ATM0`). The slot is defined again at every change of bus, which costs one
 * word and makes a reset of the chip in between harmless.
 *
 * Rows of one bus are asked together (ap_sched_next_grouped), so a round
 * changes bus once per bus, and the chip is taken back to the car's OBD
 * protocol as soon as no row of another bus is due (ap_bus_leave): an app,
 * a scan or a test that comes next finds the chip where it always was.
 *
 * The pure half (autopid_bus_core.c) is host-tested; the chip half
 * (autopid_bus.c) is the runner's third file (autopid_runner_private.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "autopid_private.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    AP_BUS_OBD = 0,   /* the car's OBD protocol (the baseline's)           */
    AP_BUS_MS,        /* MS CAN, pins 3 and 11                             */
    AP_BUS_SW,        /* single-wire CAN, pin 1, normal mode only          */
    AP_BUS_CH,        /* CH CAN, pins 12 and 13                            */
    AP_BUS_COUNT
} ap_row_bus_t;

/* Where a row asks, as one number the scheduler and the runner compare: the
 * bus and its bit rate code (0 = the bus's own rate; else bit 7 = times 8/7,
 * bits 0..6 = the divisor of 500 kbit/s). 0 = the car's OBD protocol. Rows
 * of one bus at two speeds are two places: a config that has them changes
 * bus between them. */
#define AP_BUS_KEY(bus, rate)  ((uint16_t)(((uint16_t)(bus) << 8) | (rate)))
#define AP_BUS_OF(key)         ((uint8_t)((key) >> 8))
#define AP_BUS_RATE_OF(key)    ((uint8_t)((key) & 0xFFu))
#define AP_BUS_RATE_X87        0x80u

/* ---- pure (autopid_bus_core.c, host-tested) -------------------------------- */

/** The bus a config names: NULL, "" and "obd" = AP_BUS_OBD, "ms", "sw",
 *  "ch" (lower case, as the web UI writes them). @return false = a word
 *  this firmware does not know: the row is refused, not polled on a guess. */
bool ap_bus_parse(const char *word, ap_row_bus_t *out);

/** The rate code of a bus speed in kbit/s (`bus_kbps`: 250, 83.3, 95.2):
 *  0 for no speed (<= 0) and for the bus's own; else the divisor of
 *  500 kbit/s, with AP_BUS_RATE_X87 when it takes the 8/7 multiplier.
 *  Within 1 % of what the chip makes. @return false = a speed the chip
 *  cannot make, or one on a row of the car's OBD protocol. */
bool ap_bus_rate_parse(uint8_t bus, double kbps, uint8_t *code);

/** The bit rate of a bus at a rate code, in bit/s (0 for AP_BUS_OBD). */
uint32_t ap_bus_rate_bps(uint8_t bus, uint8_t code);

/** The config word of a bus ("obd", "ms", "sw", "ch"). */
const char *ap_bus_word(uint8_t bus);

/** The bus as a log or a message names it ("MS CAN", "single-wire CAN"). */
const char *ap_bus_label(uint8_t bus);

/** The chip's AT user protocol a bus is reached through ('B', 'F', 'C');
 *  '\0' for AP_BUS_OBD. */
char ap_bus_slot(uint8_t bus);

/** The chain that takes the chip to a bus from anywhere but that bus's own
 *  slot: the print format the reader needs, the slot's definition at that
 *  rate, the slot, the functional header, the automatic receive filter.
 *  @return its length; 0 for AP_BUS_OBD (the baseline prelude is its
 *  chain) and when it does not fit @p cap (AP_INIT_LEN always does). */
size_t ap_bus_chain(uint16_t key, char *out, size_t cap);

/** May a chain of the tables (a row's init, an AT command as its cmd) go
 *  out on a row of another bus? Not when it picks a protocol, defines a
 *  slot, links a transceiver or resets the chip itself: the bus is the
 *  row's `bus` field and nothing else. NULL / "" = true. */
bool ap_bus_chain_allowed(const char *chain);

/** Where scheduler entry @p i asks (AP_BUS_KEY; a filter and a PGN row: 0,
 *  the car's OBD protocol). */
uint16_t ap_sched_entry_bus(const ap_config_t *cfg, int i);

/** A row is asked at most this long before its time to save a change of
 *  bus (and never more than a quarter of its period). */
#define AP_BUS_EARLY_MAX_US  500000

/** ap_sched_next_of with the chip's rows grouped by bus (autopid_sched.c).
 *  @p bus_now: where the chip is (AP_BUS_KEY, 0 = home, on the car's OBD
 *  protocol); @p since_us: when it came home, or when it left home (the
 *  start of the excursion). Home, the most overdue row wins as before,
 *  unless it would take the chip away while rows of the car's protocol are
 *  due or nearly due (a quarter of their period, AP_BUS_EARLY_MAX_US at
 *  most) and were not asked since it came home: those go first. Away, every
 *  row of the other buses that is due or nearly due is asked once per
 *  excursion, the bus the chip is on first, then the next bus directly.
 *  So a round changes bus once per bus and once to come home, and no bus can
 *  hold the chip while a row of another waits. A row asked early comes back
 *  with *due_us = now. */
int ap_sched_next_grouped(const ap_sched_t *st, const ap_config_t *cfg,
                          unsigned classes, uint16_t bus_now,
                          int64_t since_us, int64_t now_us, int64_t *due_us);

/* A bus nobody acknowledges on. The HS bus has the bus guard: the native
 * controller listens before the chip transmits. The other buses have no
 * listener, so a request at the wrong speed (a `bus_kbps` that is not the
 * car's, or the usual speed on a car that differs), or on a bus the car
 * does not have, is found out by sending: the chip answers `CAN ERROR`, and
 * on a live bus at another speed every such try costs that bus error
 * frames. After AP_BUS_ERR_STREAK of them in a row the bus's rows rest:
 * AP_BUS_PARK_MIN_US, doubled at every park that follows without a frame
 * getting through, AP_BUS_PARK_MAX_US at most. Any other reply (an answer,
 * NO DATA: the frame was acknowledged) ends it. */
#define AP_BUS_ERR_STREAK    3
#define AP_BUS_PARK_MIN_US   30000000LL
#define AP_BUS_PARK_MAX_US   600000000LL

typedef struct
{
    uint8_t  streak;     /* CAN ERROR replies in a row                    */
    uint32_t parks;      /* how often the bus was parked                  */
    int64_t  park_us;    /* the length of the last park (0 = none stands) */
    int64_t  until_us;   /* parked until then                             */
} ap_bus_err_t;

/** The end of a request on a bus: @p can_error = the chip said CAN ERROR.
 *  @return true = this one parks the bus (until e->until_us). */
bool ap_bus_err_note(ap_bus_err_t *e, bool can_error, int64_t now_us);

/** Do the rows of that bus rest right now? */
bool ap_bus_err_parked(const ap_bus_err_t *e, int64_t now_us);

#ifndef AUTOPID_HOST_TEST
/* ---- the chip half (autopid_bus.c) ----------------------------------------- */

/** Where the chip was taken to (AP_BUS_KEY; 0 = the car's OBD protocol),
 *  and since when it is home or, away, when it left home (esp_timer µs):
 *  what the scheduler groups the rows by. */
uint16_t ap_bus_now(void);
int64_t ap_bus_since_us(void);

/** A row of another bus is next: the chip onto its bus when it is not there
 *  (or was reset since), then the row's own init and receive filter.
 *  @return false = nothing sent that leaves the chip on that bus without
 *  its setup (the slot was not taken), or the bus's rows rest after CAN
 *  ERRORs (ap_bus_err_t; not for a test, @p pid_index < 0): the row is not
 *  asked. */
bool ap_bus_row_setup(const char *name, uint16_t key, int pid_index,
                      const char *init, const char *rxheader, ap_tr_t *tr);

/** Back to the car's OBD protocol when the chip is on another bus: the
 *  baseline prelude when the bus guard's verdict covers it, else the
 *  protocol alone (no frame goes out for either). The type and row inits are
 *  due again after it. No-op on AP_BUS_OBD. */
void ap_bus_leave(ap_tr_t *tr);

/** Counters for GET /api/autopid: changes of bus, and per bus the rows
 *  answered and not. */
typedef struct
{
    uint32_t changes;
    uint32_t ok[AP_BUS_COUNT];
    uint32_t failed[AP_BUS_COUNT];
    uint32_t parks[AP_BUS_COUNT];   /* rests after CAN ERRORs (ap_bus_err_t) */
    bool     parked[AP_BUS_COUNT];  /* resting right now                    */
} ap_bus_stats_t;

void ap_bus_note(uint8_t bus, bool ok);

/** What the chip said to a request on another bus (the raw reply): feeds
 *  the CAN ERROR count of that bus. */
void ap_bus_reply(uint8_t bus, const char *resp);
void ap_bus_get_stats(ap_bus_stats_t *out);
#endif

#ifdef __cplusplus
}
#endif
