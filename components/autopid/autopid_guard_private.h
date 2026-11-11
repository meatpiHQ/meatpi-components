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
 * @file autopid_guard_private.h
 * @brief The bus guard's pure half of autopid_private.h: may the OBD chip
 *        transmit on a protocol, given what the native controller heard
 *        on the bus (autopid_bus_guard.c, pure).
 *        A FRAGMENT: included by autopid_private.h only (it needs its
 *        base types); split out 2026-11-11 to keep autopid_private.h
 *        under the 700-line rule.
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

/* ---- pure: bus guard (autopid_bus_guard.c, host-tested) -------------------
 * May the OBD chip transmit on a protocol, given what the native
 * controller heard on the bus (can_manager_probe)? A request on a pinned
 * CAN protocol at the wrong bitrate destroys the bus traffic; the chip's
 * own search does not. */
typedef enum
{
    AP_BUS_UNKNOWN = 0,   /* not probed, or the probe was not possible      */
    AP_BUS_SILENT,        /* nothing on the bus (a gatewayed OBD port)      */
    AP_BUS_LIVE,          /* frames read at `kbps`                          */
    AP_BUS_UNREADABLE,    /* traffic that neither 250 nor 500 kbit/s reads  */
} ap_bus_kind_t;

typedef struct
{
    ap_bus_kind_t kind;
    uint16_t      kbps;   /* AP_BUS_LIVE only */
} ap_bus_t;

typedef enum
{
    AP_GUARD_ALLOW = 0,   /* send on the protocol asked for                 */
    AP_GUARD_SEARCH,      /* not on that one: the chip's search (ATTP0)     */
    AP_GUARD_PARK,        /* nothing is transmitted                         */
} ap_guard_verdict_t;

/** kbit/s the chip transmits at on CAN protocol @p proto ('6'..'9', 'A');
 *  0 = not judged (the search '0', K-line / J1850, user CAN B and C). */
uint16_t ap_guard_proto_kbps(char proto);

/** The decision. @p proto = the protocol the prelude would pin ('0' = the
 *  search); @p pinned = the `std_protocol` SETTING names it (the user's
 *  word), not the vehicle store (ours, which may give way to the search). */
ap_guard_verdict_t ap_guard_decide(const ap_bus_t *bus, char proto,
                                   bool pinned);

/** May a prelude pin @p proto now? @p ruled = the protocol the last positive
 *  verdict was taken for ('0' = the search, '\0' = none stands). A verdict
 *  is about ONE protocol on the bus as it was looked at: when the protocol
 *  in effect changes between the look and the prelude (another car made
 *  current), the prelude waits for the next look. Bench 2026-10-05: a 500
 *  kbit/s car activated on a 250 kbit/s truck was pinned on the strength of
 *  the truck's verdict, and the truck's adapter went bus-off in 40 ms. */
bool ap_guard_pin_allowed(char proto, char ruled);

/** The CAN protocol an AT command sets, as the chip reads it (`ATSP6`,
 *  `at tp a7`, `ATSP00`): '0'..'9', 'A'..'C'; '\0' = the command sets none. */
char ap_guard_cmd_proto(const char *cmd, size_t len);

/** True for the commands that reset the chip (`ATZ`, `ATD`, `ATWS`): it
 *  comes back on the protocol stored in its EEPROM, the last vehicle
 *  DETECTED, whatever bus the device is plugged into now. Whoever sends one
 *  out of a chain of the tables sends the baseline prelude right behind it. */
bool ap_guard_cmd_resets(const char *cmd, size_t len);

/** A ';'-separated chain of the tables (a row's or a type's init, a cmd,
 *  dtc_init): is EVERY protocol it sets allowed on @p bus, by the ruling a
 *  pinned setting gets (no search to give way to)? @p last (may be NULL) =
 *  the protocol the chain leaves the chip on, '\0' when it sets none (or
 *  resets the chip after the last one it set);
 *  @p refused (may be NULL) = the first one refused. Bench 2026-10-05: a row
 *  with `ATSP6` tested on a 250 kbit/s truck, the truck's adapter bus-off
 *  19 ms after its first error. */
bool ap_guard_chain_allowed(const ap_bus_t *bus, const char *chain,
                            char *last, char *refused);

/** The sentence of such a refusal: "<owner> sets protocol 6 (500 kbit/s) and
 *  the vehicle bus runs at 250 kbit/s: not sent". @return its length. */
size_t ap_guard_chain_reason(const ap_bus_t *bus, char proto,
                             const char *owner, char *out, size_t cap);

const char *ap_guard_verdict_name(ap_guard_verdict_t verdict);
const char *ap_bus_kind_name(ap_bus_kind_t kind);

/** One sentence for the log and the status: what the bus is and what that
 *  means for @p proto. @return its length (0 on a NULL / empty buffer). */
size_t ap_guard_reason(const ap_bus_t *bus, char proto, bool pinned,
                       char *out, size_t cap);
