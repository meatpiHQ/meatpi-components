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
 * @file autopid_bus_guard.c
 * @brief PURE: may the OBD chip transmit on this protocol, given what is on
 *        the bus? (host-tested)
 *
 * The chip's own protocol search is safe on a live bus (it matches the bus
 * frequency before it sends: 0 error frames, bench 2026-10-02). A request on
 * a PINNED protocol at the other bitrate is not: a ~65 ms burst, 257 to 1255
 * error frames, the sending ECU driven to bus-off, and the poller repeats it
 * every period. So before the chip is pinned to a CAN protocol the native
 * controller listens (can_manager_probe, it cannot transmit while it does)
 * and this decides:
 *
 *   bus silent                   as before: the protocol asked for
 *   bus live at the same rate    the protocol asked for
 *   bus live at the other rate   the vehicle store's protocol gives way to
 *                                the chip's search; a protocol pinned by the
 *                                `std_protocol` setting does not transmit
 *   bus live, unreadable         nothing is transmitted (not even a search:
 *                                the chip was only measured at 250 and 500)
 */
#include <stdio.h>

#include "autopid_private.h"

uint16_t ap_guard_proto_kbps(char proto)
{
    switch (proto)
    {
    case '6':
    case '7':
        return 500;

    case '8':
    case '9':
    case 'A':
    case 'a':
        return 250;

    default:
        return 0; /* not CAN (1..5), the search ('0'), user CAN (B, C) */
    }
}

ap_guard_verdict_t ap_guard_decide(const ap_bus_t *bus, char proto,
                                   bool pinned)
{
    if (bus == NULL || bus->kind == AP_BUS_UNKNOWN ||
        bus->kind == AP_BUS_SILENT)
    {
        return AP_GUARD_ALLOW; /* nothing known, or nothing to disturb */
    }

    bool is_search = (proto == '0' || proto == '\0');
    uint16_t kbps = ap_guard_proto_kbps(proto);

    if (!is_search && kbps == 0)
    {
        /* K-line / J1850 use other pins; B and C are the user's own CAN
           definitions, which this table cannot judge */
        return AP_GUARD_ALLOW;
    }

    if (bus->kind == AP_BUS_UNREADABLE)
    {
        return AP_GUARD_PARK;
    }

    /* live at a known bitrate */
    if (is_search || kbps == bus->kbps)
    {
        return AP_GUARD_ALLOW;
    }

    return pinned ? AP_GUARD_PARK : AP_GUARD_SEARCH;
}

const char *ap_guard_verdict_name(ap_guard_verdict_t verdict)
{
    switch (verdict)
    {
    case AP_GUARD_SEARCH: return "search";
    case AP_GUARD_PARK:   return "park";
    default:              return "allow";
    }
}

const char *ap_bus_kind_name(ap_bus_kind_t kind)
{
    switch (kind)
    {
    case AP_BUS_SILENT:     return "silent";
    case AP_BUS_LIVE:       return "live";
    case AP_BUS_UNREADABLE: return "unreadable";
    default:                return "unknown";
    }
}

size_t ap_guard_reason(const ap_bus_t *bus, char proto, bool pinned,
                       char *out, size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return 0;
    }

    ap_guard_verdict_t v = ap_guard_decide(bus, proto, pinned);
    unsigned bus_kbps = (bus != NULL) ? bus->kbps : 0;
    unsigned proto_kbps = ap_guard_proto_kbps(proto);
    int n;

    if (v == AP_GUARD_PARK && bus != NULL && bus->kind == AP_BUS_UNREADABLE)
    {
        n = snprintf(out, cap, "the vehicle bus carries traffic at a "
                               "bitrate that could not be read (not 250 "
                               "or 500 kbit/s): nothing is transmitted");
    }
    else if (v == AP_GUARD_PARK)
    {
        n = snprintf(out, cap, "the vehicle bus runs at %u kbit/s and the "
                               "protocol setting %c transmits at %u: "
                               "nothing is transmitted (set the protocol "
                               "to Automatic)",
                     bus_kbps, proto, proto_kbps);
    }
    else if (v == AP_GUARD_SEARCH)
    {
        n = snprintf(out, cap, "the vehicle bus runs at %u kbit/s, the "
                               "stored protocol %c transmits at %u: using "
                               "the chip's protocol search instead",
                     bus_kbps, proto, proto_kbps);
    }
    else if (bus != NULL && bus->kind == AP_BUS_LIVE)
    {
        n = snprintf(out, cap, "vehicle bus live at %u kbit/s", bus_kbps);
    }
    else if (bus != NULL && bus->kind == AP_BUS_SILENT)
    {
        n = snprintf(out, cap, "vehicle bus silent");
    }
    else
    {
        n = snprintf(out, cap, "vehicle bus not probed");
    }

    (void)pinned;

    if (n < 0)
    {
        out[0] = '\0';
        return 0;
    }

    return ((size_t)n < cap) ? (size_t)n : cap - 1;
}
