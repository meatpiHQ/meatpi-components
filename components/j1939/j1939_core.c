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
 * @file j1939_core.c
 * @brief The J1939 identifier, frame kinds, the "is this a J1939 bus"
 *        evidence and the VIN message. Pure: no IDF, no clock.
 */
#include "j1939_core.h"

#include <string.h>

#define ID_EDP_BIT   (1u << 25) /* extended data page: never set on J1939    */
#define PDU1_PF_MAX  239u       /* PF below 240 carries a destination        */

/* ISO 15765 on 29-bit identifiers (J1939-21 reserves these groups for it):
 * normal fixed addressing DA00 (physical) / DB00 (functional), mixed
 * addressing CE00 / CD00 */
#define PGN_ISO15765_PHYS       0x00DA00u
#define PGN_ISO15765_FUNC       0x00DB00u
#define PGN_ISO15765_MIXED_PHYS 0x00CE00u
#define PGN_ISO15765_MIXED_FUNC 0x00CD00u

/* Groups that every J1939 vehicle broadcasts some of: the evidence that a
 * bus speaks J1939. One bit each in j1939_bus_evidence_t.known_mask, so 32
 * at most. */
static const uint32_t KNOWN_GROUPS[] =
{
    0x00F001u, /* EBC1    brake controller                      */
    0x00F002u, /* ETC1    transmission controller 1             */
    0x00F003u, /* EEC2    accelerator pedal, engine load        */
    0x00F004u, /* EEC1    engine speed, torque                  */
    0x00F005u, /* ETC2    gears                                 */
    0x00FE56u, /* AT1T1I  diesel exhaust fluid tank             */
    0x00FE6Cu, /* TCO1    tachograph                            */
    0x00FEBFu, /* EBC2    wheel speeds                          */
    0x00FEC1u, /* VDHR    high resolution vehicle distance      */
    0x00FECAu, /* DM1     active trouble codes                  */
    0x00FEDFu, /* EEC3    friction torque                       */
    0x00FEE0u, /* VD      vehicle distance                      */
    0x00FEE5u, /* HOURS   engine hours                          */
    0x00FEE6u, /* TD      time and date                         */
    0x00FEE9u, /* LFC     fuel consumption                      */
    0x00FEEEu, /* ET1     engine temperature 1                  */
    0x00FEEFu, /* EFL/P1  engine fluid level and pressure 1     */
    0x00FEF1u, /* CCVS1   cruise control, vehicle speed         */
    0x00FEF2u, /* LFE1    fuel economy                          */
    0x00FEF5u, /* AMB     ambient conditions                    */
    0x00FEF6u, /* IC1     intake and exhaust conditions 1       */
    0x00FEF7u, /* VEP1    vehicle electrical power 1            */
    0x00FEFCu, /* DD1     dash display 1                        */
    J1939_PGN_CLAIM,
};

#define KNOWN_COUNT (sizeof(KNOWN_GROUPS) / sizeof(KNOWN_GROUPS[0]))

/* ---- identifier -------------------------------------------------------------- */

bool j1939_pgn_is_pdu1(uint32_t pgn)
{
    return ((pgn >> 8) & 0xFFu) <= PDU1_PF_MAX;
}

void j1939_id_parse(uint32_t can_id, j1939_id_t *out)
{
    uint32_t raw = (can_id >> 8) & 0x3FFFFu;

    out->prio = (uint8_t)((can_id >> 26) & 0x7u);
    out->sa = (uint8_t)(can_id & 0xFFu);

    if (j1939_pgn_is_pdu1(raw))
    {
        out->pgn = raw & 0x3FF00u;
        out->da = (uint8_t)(raw & 0xFFu);
    }
    else
    {
        out->pgn = raw;
        out->da = J1939_ADDR_GLOBAL;
    }
}

uint32_t j1939_id_make(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da)
{
    pgn &= 0x1FFFFu; /* extended data page stays 0 */

    if (j1939_pgn_is_pdu1(pgn))
    {
        pgn = (pgn & 0x1FF00u) | da;
    }

    return ((uint32_t)(prio & 0x7u) << 26) | (pgn << 8) | sa;
}

j1939_kind_t j1939_classify(uint32_t can_id, bool ext, bool rtr,
                            j1939_id_t *out)
{
    j1939_id_t id;

    if (!ext || rtr || (can_id & ID_EDP_BIT) != 0)
    {
        return J1939_KIND_FOREIGN;
    }

    j1939_id_parse(can_id, &id);

    if (out != NULL)
    {
        *out = id;
    }

    switch (id.pgn)
    {
    case PGN_ISO15765_PHYS:
    case PGN_ISO15765_FUNC:
    case PGN_ISO15765_MIXED_PHYS:
    case PGN_ISO15765_MIXED_FUNC:
        return J1939_KIND_DIAG;

    case J1939_PGN_TP_CM:
        return J1939_KIND_TP_CM;

    case J1939_PGN_TP_DT:
        return J1939_KIND_TP_DT;

    default:
        return J1939_KIND_DATA;
    }
}

/* ---- bus evidence -------------------------------------------------------------- */

void j1939_bus_note(j1939_bus_evidence_t *ev, uint32_t can_id, bool ext,
                    bool rtr, uint8_t dlc)
{
    j1939_id_t id;

    ev->frames++;

    switch (j1939_classify(can_id, ext, rtr, &id))
    {
    case J1939_KIND_FOREIGN:
        ev->foreign++;
        return;

    case J1939_KIND_DIAG:
        ev->diag++;
        return;

    case J1939_KIND_DATA:
        break;

    default:
        return; /* transport frames: neither for nor against */
    }

    if (dlc != 8)
    {
        return; /* every group of the list is 8 bytes long */
    }

    for (size_t i = 0; i < KNOWN_COUNT; i++)
    {
        if (KNOWN_GROUPS[i] == id.pgn)
        {
            ev->known++;
            ev->known_mask |= (1u << i);
            return;
        }
    }
}

j1939_bus_t j1939_bus_verdict(const j1939_bus_evidence_t *ev)
{
    unsigned groups = 0;

    for (uint32_t m = ev->known_mask; m != 0; m &= (m - 1))
    {
        groups++;
    }

    if (groups >= J1939_BUS_MIN_GROUPS)
    {
        return J1939_BUS_J1939;
    }

    if (groups == 0 && ev->frames >= J1939_BUS_OTHER_AFTER)
    {
        return J1939_BUS_OTHER;
    }

    return J1939_BUS_UNKNOWN;
}

const char *j1939_bus_name(j1939_bus_t bus)
{
    switch (bus)
    {
    case J1939_BUS_J1939: return "j1939";
    case J1939_BUS_OTHER: return "other";
    default:              return "unknown";
    }
}

/* ---- vehicle identification ------------------------------------------------------ */

static bool vin_char_ok(uint8_t c)
{
    if (c >= '0' && c <= '9')
    {
        return true;
    }

    return c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q';
}

bool j1939_vin_parse(const uint8_t *data, size_t len,
                     char out[J1939_VIN_LEN + 1])
{
    size_t n = 0;

    if (data == NULL || out == NULL)
    {
        return false;
    }

    /* up to the delimiter, or to the end when there is none */
    while (n < len && data[n] != '*')
    {
        n++;
    }

    /* padding some controllers leave behind the characters */
    while (n > 0 && (data[n - 1] == ' ' || data[n - 1] == 0x00 ||
                     data[n - 1] == 0xFF))
    {
        n--;
    }

    if (n != J1939_VIN_LEN)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        if (!vin_char_ok(data[i]))
        {
            return false;
        }
    }

    memcpy(out, data, J1939_VIN_LEN);
    out[J1939_VIN_LEN] = '\0';
    return true;
}

/* ---- request and acknowledgment ------------------------------------------------- */

static uint32_t pgn_le(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
}

static void pgn_le_put(uint32_t pgn, uint8_t *d)
{
    d[0] = (uint8_t)pgn;
    d[1] = (uint8_t)(pgn >> 8);
    d[2] = (uint8_t)((pgn >> 16) & 0x03u);
}

void j1939_request_build(uint32_t pgn, uint8_t out[J1939_REQUEST_LEN])
{
    pgn_le_put(pgn, out);
}

bool j1939_request_parse(const uint8_t *d, uint8_t dlc, uint32_t *pgn)
{
    if (d == NULL || dlc < J1939_REQUEST_LEN)
    {
        return false;
    }

    if (pgn != NULL)
    {
        *pgn = pgn_le(d) & 0x3FFFFu;
    }

    return true;
}

void j1939_ackm_build(uint8_t control, uint8_t address, uint32_t pgn,
                      uint8_t out[8])
{
    out[0] = control;
    out[1] = 0xFF; /* group function: none */
    out[2] = 0xFF;
    out[3] = 0xFF;
    out[4] = address;
    pgn_le_put(pgn, &out[5]);
}

bool j1939_ackm_parse(const uint8_t *d, uint8_t dlc, j1939_ackm_t *out)
{
    if (d == NULL || dlc < 8)
    {
        return false;
    }

    if (out != NULL)
    {
        out->control = d[0];
        out->group_function = d[1];
        out->address = d[4];
        out->pgn = pgn_le(&d[5]) & 0x3FFFFu;
    }

    return true;
}

const char *j1939_ack_name(uint8_t control)
{
    switch (control)
    {
    case J1939_ACK_POSITIVE: return "ack";
    case J1939_ACK_NEGATIVE: return "nack";
    case J1939_ACK_DENIED:   return "denied";
    case J1939_ACK_BUSY:     return "busy";
    default:                 return "?";
    }
}
