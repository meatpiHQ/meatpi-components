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
 * @file obd_chip_class.c
 * @brief Two small classifiers of the request engine (pure, no IDF deps,
 *        host-tested), kept out of obd_chip_parse.c, which is at the
 *        700-line limit: which words make the chip transmit on the vehicle
 *        bus, and which protocol an `ATDPN` answer names.
 *
 * The request engine holds the bus words back while a protocol switch is owed
 * (obd_chip_cmd.c, 2026-11-10): a switch the driver refused left the chip on
 * the protocol of whoever used it before, and a request sent then goes out
 * on that protocol, at that bit rate. A word that only sets the chip (a
 * header, a filter, a timeout) changes nothing on the bus and passes.
 */
#include "obd_chip_private.h"

#include <ctype.h>
#include <string.h>

bool obd_parse_dpn(const char *resp, char *proto, bool *automatic)
{
    /* `6`, `A6` (automatic, on 6), `A` (J1939), `AA`; blanks and line ends
       around it. The STN and vendor presets print more (`34 [ST]`): not one
       of the ELM numbers the save takes. */
    char c[4];
    size_t n = 0;

    if (resp == NULL)
    {
        return false;
    }

    for (const char *p = resp; *p != '\0'; p++)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        {
            continue;
        }

        if (n >= sizeof(c) - 1)
        {
            return false;
        }

        c[n++] = (char)toupper((unsigned char)*p);
    }

    c[n] = '\0';

    bool is_auto = (n == 2 && c[0] == 'A');
    char p = (n == 1) ? c[0] : is_auto ? c[1] : '\0';

    if (!((p >= '0' && p <= '9') || (p >= 'A' && p <= 'C')))
    {
        return false;
    }

    if (proto != NULL)
    {
        *proto = p;
    }

    if (automatic != NULL)
    {
        *automatic = is_auto;
    }

    return true;
}

bool obd_parse_is_bus_request(const char *cmd)
{
    /* as the chip reads it: spaces dropped, any case, up to the CR (a ';'
       ends it too: the first element of a chain) */
    char c[16];
    size_t n = 0;

    if (cmd == NULL)
    {
        return false;
    }

    for (const char *p = cmd;
         *p != '\0' && *p != '\r' && *p != '\n' && *p != ';' &&
         n < sizeof(c) - 1; p++)
    {
        if (*p == ' ' || *p == '\t')
        {
            continue;
        }

        c[n++] = (char)toupper((unsigned char)*p);
    }

    c[n] = '\0';

    if (n == 0)
    {
        return true; /* a bare CR repeats the last command, a request maybe */
    }

    if (strncmp(c, "AT", 2) == 0)
    {
        /* the K-line starts and the remote frame are sent on the bus; every
           other AT word sets or reads the chip */
        return strncmp(c, "ATFI", 4) == 0 || strncmp(c, "ATSI", 4) == 0 ||
               strncmp(c, "ATRTR", 5) == 0;
    }

    if (strncmp(c, "ST", 2) == 0)
    {
        return strncmp(c, "STPX", 4) == 0; /* the STN transmit word */
    }

    if (strncmp(c, "VT", 2) == 0)
    {
        return strncmp(c, "VTFULLYREQUEST", 14) == 0; /* the vendor's */
    }

    return true; /* hex data, or anything the chip would try to send */
}
