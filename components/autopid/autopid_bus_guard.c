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
 *
 * The tables pin protocols too (`ATSP6` in a row's init, a type's init,
 * `dtc_init`, a row tested from the UI: most vehicle profiles carry one), and
 * such a chain goes to the chip as written. ap_guard_chain_allowed() gives
 * every protocol a chain sets the same ruling as a pinned setting: on a bus
 * that runs at another bit rate the chain, and the row it belongs to, is not
 * sent (2026-10-05).
 */
#include <stdio.h>
#include <string.h>

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

bool ap_guard_pin_allowed(char proto, char ruled)
{
    /* K-line / J1850 use other pins, B and C are the user's own CAN
       definitions: not this guard's to judge (as in ap_guard_decide) */
    if (proto != '0' && proto != '\0' && ap_guard_proto_kbps(proto) == 0)
    {
        return true;
    }

    /* a CAN protocol or the chip's search: only the one the bus was looked
       at for. Another one (another car made current a moment ago) has had
       no look yet, whatever the last verdict said. */
    return ruled != '\0' && proto == ruled;
}

char ap_guard_cmd_proto(const char *cmd, size_t len)
{
    /* as the chip reads a command: no spaces, any case. The longest form
       is "ATSPAh" */
    char c[7];
    size_t n = 0;

    if (cmd == NULL)
    {
        return '\0';
    }

    for (size_t i = 0; i < len && cmd[i] != '\0'; i++)
    {
        char ch = cmd[i];

        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            continue;
        }

        if (n >= sizeof(c) - 1)
        {
            return '\0'; /* longer than any form of ATSP / ATTP */
        }

        c[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }

    if ((n != 5 && n != 6) || c[0] != 'A' || c[1] != 'T' ||
        (c[2] != 'S' && c[2] != 'T') || c[3] != 'P')
    {
        return '\0';
    }

    /* ATSPh; ATSPAh (h first, the search when h stays silent: the first
       request still goes out on h); ATSP00 (the search) */
    char p = c[n - 1];

    if (n == 6 && c[4] != 'A' && !(c[4] == '0' && p == '0'))
    {
        return '\0';
    }

    return ((p >= '0' && p <= '9') || (p >= 'A' && p <= 'C')) ? p : '\0';
}

bool ap_guard_cmd_resets(const char *cmd, size_t len)
{
    /* ATZ, ATD (all to defaults), ATWS (warm start): each one loads the
       protocol stored in the chip's EEPROM. Not ATDP / ATDPN / ATD0 / ATD1. */
    char c[5];
    size_t n = 0;

    if (cmd == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < len && cmd[i] != '\0'; i++)
    {
        char ch = cmd[i];

        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            continue;
        }

        if (n >= sizeof(c) - 1)
        {
            return false;
        }

        c[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }

    c[n] = '\0';
    return strcmp(c, "ATZ") == 0 || strcmp(c, "ATD") == 0 ||
           strcmp(c, "ATWS") == 0;
}

bool ap_guard_chain_allowed(const ap_bus_t *bus, const char *chain,
                            char *last, char *refused)
{
    bool ok = true;

    if (last != NULL)
    {
        *last = '\0';
    }

    if (refused != NULL)
    {
        *refused = '\0';
    }

    for (const char *p = chain; p != NULL && *p != '\0';)
    {
        const char *sep = strchr(p, ';');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char proto = ap_guard_cmd_proto(p, len);

        if (last != NULL && ap_guard_cmd_resets(p, len))
        {
            *last = '\0'; /* the sender puts the baseline prelude behind a
                             reset: what the chain set before it is gone */
        }

        if (proto != '\0')
        {
            if (last != NULL)
            {
                *last = proto;
            }

            /* the chain's word is its author's, as a pinned setting is the
               user's: there is no search to give way to. EVERY protocol of
               the chain counts, not the last one: a chain may carry a
               request between two of them (a session opener) */
            if (ok && ap_guard_decide(bus, proto, true) != AP_GUARD_ALLOW)
            {
                ok = false;

                if (refused != NULL)
                {
                    *refused = proto;
                }
            }
        }

        p += len + ((sep != NULL) ? 1 : 0);
    }

    return ok;
}

size_t ap_guard_chain_reason(const ap_bus_t *bus, char proto,
                             const char *owner, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return 0;
    }

    int n;

    if (owner == NULL)
    {
        owner = "an init";
    }

    if (bus != NULL && bus->kind == AP_BUS_UNREADABLE)
    {
        n = snprintf(out, cap, "%s sets protocol %c and the vehicle bus "
                               "carries traffic at a bitrate that could not "
                               "be read: not sent", owner, proto);
    }
    else
    {
        n = snprintf(out, cap, "%s sets protocol %c (%u kbit/s) and the "
                               "vehicle bus runs at %u kbit/s: not sent",
                     owner, proto, (unsigned)ap_guard_proto_kbps(proto),
                     (bus != NULL) ? (unsigned)bus->kbps : 0u);
    }

    if (n < 0)
    {
        out[0] = '\0';
        return 0;
    }

    return ((size_t)n < cap) ? (size_t)n : cap - 1;
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
