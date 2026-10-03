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
 * @file j1939_dm_core.c
 * @brief DM1 / DM2 payload decode (J1939-73). Pure.
 */
#include "j1939_dm_core.h"

#include <stdio.h>

#define DM_HEADER_BYTES 2u
#define DM_RECORD_BYTES 4u
#define SPN_NA          0x7FFFFu
#define FMI_NA          31u

size_t j1939_dm_parse(const uint8_t *p, size_t len, j1939_lamps_t *lamps,
                      j1939_dtc_t *out, size_t max)
{
    size_t n = 0;

    if (lamps != NULL)
    {
        lamps->mil = J1939_LAMP_NA;
        lamps->rsl = J1939_LAMP_NA;
        lamps->awl = J1939_LAMP_NA;
        lamps->pl = J1939_LAMP_NA;
        lamps->flash = 0xFF;
    }

    if (p == NULL || len < DM_HEADER_BYTES)
    {
        return 0;
    }

    if (lamps != NULL)
    {
        lamps->mil = (p[0] >> 6) & 0x3u;
        lamps->rsl = (p[0] >> 4) & 0x3u;
        lamps->awl = (p[0] >> 2) & 0x3u;
        lamps->pl = p[0] & 0x3u;
        lamps->flash = p[1];
    }

    /* whole records only: the FF FF that fills a frame is not one */
    for (size_t i = DM_HEADER_BYTES; i + DM_RECORD_BYTES <= len;
         i += DM_RECORD_BYTES)
    {
        uint32_t spn = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
                       ((uint32_t)(p[i + 2] >> 5) << 16);
        uint8_t fmi = p[i + 2] & 0x1Fu;
        uint8_t oc = p[i + 3] & 0x7Fu;
        bool cm = (p[i + 3] & 0x80u) != 0;

        if (spn == 0 && fmi == 0 && oc == 0 && !cm)
        {
            continue; /* the "no trouble code" record */
        }

        if (spn == SPN_NA && fmi == FMI_NA)
        {
            continue; /* a record of ones: filler */
        }

        if (out != NULL && n < max)
        {
            out[n].spn = spn;
            out[n].fmi = fmi;
            out[n].oc = oc;
            out[n].cm = cm;
        }

        n++;
    }

    return n;
}

void j1939_dtc_text(uint32_t spn, uint8_t fmi, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return;
    }

    snprintf(out, cap, "SPN%lu-%u", (unsigned long)(spn & SPN_NA),
             (unsigned)(fmi & 0x1Fu));
}
