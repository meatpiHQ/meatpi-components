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
 * @file j1939_spn_core.c
 * @brief The built-in J1939-71 value table and its decode. Pure.
 *
 * Layouts written from public descriptions of J1939-71 and cross-checked on
 * the bench against tools/testbench/lib/j1939_ref.py (itself checked against
 * the DBC fixture). max = the largest valid raw value (FA, FAFF, FAFFFFFF)
 * decoded, so a consumer that clamps to [min, max] drops "error" and "not
 * available" by itself.
 */
#include "j1939_spn_core.h"

#include <string.h>

#define RAW8_MAX  250.0        /* 0xFA       */
#define RAW16_MAX 64255.0      /* 0xFAFF     */
#define RAW32_MAX 4211081215.0 /* 0xFAFFFFFF */

/* the parameter groups of the table */
#define EEC1   0x00F004u /* electronic engine controller 1 (61444)       */
#define EEC2   0x00F003u /* electronic engine controller 2 (61443)       */
#define ETC2   0x00F005u /* electronic transmission controller 2 (61445) */
#define CCVS1  0x00FEF1u /* cruise control, vehicle speed (65265)        */
#define ET1    0x00FEEEu /* engine temperature 1 (65262)                 */
#define EFLP1  0x00FEEFu /* engine fluid level and pressure 1 (65263)    */
#define IC1    0x00FEF6u /* intake and exhaust conditions 1 (65270)      */
#define AMB    0x00FEF5u /* ambient conditions (65269)                   */
#define VEP1   0x00FEF7u /* vehicle electrical power 1 (65271)           */
#define LFE1   0x00FEF2u /* fuel economy (65266)                         */
#define DD1    0x00FEFCu /* dash display 1 (65276)                       */
#define AT1T1I 0x00FE56u /* diesel exhaust fluid tank 1 (65110)          */
#define VDHR   0x00FEC1u /* high resolution vehicle distance (65217)     */
#define VD     0x00FEE0u /* vehicle distance (65248), on request         */
#define HOURS  0x00FEE5u /* engine hours (65253), on request             */
#define LFC    0x00FEE9u /* fuel consumption (65257), on request         */

/* one, two or four bytes starting at data byte B (from 0) */
#define SPN_ROW(spn, pgn, name, unit, cls, b, bits, raw_max, scale, off)      \
    {                                                                         \
        spn, pgn, name, unit, cls, b, 0, bits, scale, off, off,               \
        (raw_max) * (scale) + (off)                                           \
    }
#define SPN8(spn, pgn, name, unit, cls, b, scale, off)                        \
    SPN_ROW(spn, pgn, name, unit, cls, b, 8, RAW8_MAX, scale, off)
#define SPN16(spn, pgn, name, unit, cls, b, scale, off)                       \
    SPN_ROW(spn, pgn, name, unit, cls, b, 16, RAW16_MAX, scale, off)
#define SPN32(spn, pgn, name, unit, cls, b, scale, off)                       \
    SPN_ROW(spn, pgn, name, unit, cls, b, 32, RAW32_MAX, scale, off)

#define T "temperature"
#define P "pressure"

/* clang-format off */
static const j1939_spn_t TABLE[] =
{
    SPN16(190,  EEC1,   "EngineSpeed",               "rpm",   "none",     3, 0.125,   0.0),
    SPN8 (513,  EEC1,   "ActualEnginePercentTorque", "%",     "none",     2, 1.0,     -125.0),
    SPN8 (91,   EEC2,   "AccelPedalPosition1",       "%",     "none",     1, 0.4,     0.0),
    SPN8 (92,   EEC2,   "EnginePercentLoad",         "%",     "power_factor", 2, 1.0, 0.0),
    SPN8 (523,  ETC2,   "TransCurrentGear",          "none",  "none",     3, 1.0,     -125.0),
    SPN16(84,   CCVS1,  "WheelBasedVehicleSpeed",    "km/h",  "speed",    1, 1.0 / 256.0, 0.0),
    SPN8 (110,  ET1,    "EngineCoolantTemperature",  "degC",  T,          0, 1.0,     -40.0),
    SPN16(175,  ET1,    "EngineOilTemperature",      "degC",  T,          2, 0.03125, -273.0),
    SPN8 (100,  EFLP1,  "EngineOilPressure",         "kPa",   P,          3, 4.0,     0.0),
    SPN8 (102,  IC1,    "IntakeManifoldPressure",    "kPa",   P,          1, 2.0,     0.0),
    SPN8 (105,  IC1,    "IntakeManifoldTemperature", "degC",  T,          2, 1.0,     -40.0),
    SPN8 (108,  AMB,    "BarometricPressure",        "kPa",   "atmospheric_pressure",
                                                                          0, 0.5,     0.0),
    SPN16(171,  AMB,    "AmbientAirTemperature",     "degC",  T,          3, 0.03125, -273.0),
    SPN16(168,  VEP1,   "BatteryPotential",          "V",     "voltage",  4, 0.05,    0.0),
    SPN16(183,  LFE1,   "FuelRate",                  "L/h",   "none",     0, 0.05,    0.0),
    SPN8 (96,   DD1,    "FuelLevel1",                "%",     "none",     1, 0.4,     0.0),
    SPN8 (1761, AT1T1I, "DEFTankLevel",              "%",     "none",     0, 0.4,     0.0),
    SPN32(917,  VDHR,   "TotalVehicleDistanceHR",    "km",    "distance", 0, 0.005,   0.0),
    SPN32(245,  VD,     "TotalVehicleDistance",      "km",    "distance", 4, 0.125,   0.0),
    SPN32(247,  HOURS,  "EngineTotalHours",          "hours", "duration", 0, 0.05,    0.0),
    SPN32(250,  LFC,    "EngineTotalFuelUsed",       "L",     "none",     4, 0.5,     0.0),
};
/* clang-format on */

#define TABLE_COUNT (sizeof(TABLE) / sizeof(TABLE[0]))

j1939_raw_t j1939_raw_class(uint32_t raw, uint8_t bits)
{
    if (bits >= 8 && bits <= 32)
    {
        switch ((raw >> (bits - 8u)) & 0xFFu)
        {
        case 0xFF: return J1939_RAW_NOT_AVAILABLE;
        case 0xFE: return J1939_RAW_ERROR;
        case 0xFD:
        case 0xFC: return J1939_RAW_RESERVED;
        case 0xFB: return J1939_RAW_SPECIFIC;
        default:   return J1939_RAW_VALID;
        }
    }

    if (bits >= 2 && bits < 8)
    {
        uint32_t ones = (1u << bits) - 1u;

        if ((raw & ones) == ones)
        {
            return J1939_RAW_NOT_AVAILABLE;
        }

        if ((raw & ones) == ones - 1u)
        {
            return J1939_RAW_ERROR;
        }
    }

    return J1939_RAW_VALID;
}

const char *j1939_raw_name(j1939_raw_t r)
{
    switch (r)
    {
    case J1939_RAW_VALID:         return "valid";
    case J1939_RAW_SPECIFIC:      return "specific";
    case J1939_RAW_RESERVED:      return "reserved";
    case J1939_RAW_ERROR:         return "error";
    case J1939_RAW_NOT_AVAILABLE: return "na";
    default:                      return "short";
    }
}

const j1939_spn_t *j1939_spn_table(size_t *count)
{
    if (count != NULL)
    {
        *count = TABLE_COUNT;
    }

    return TABLE;
}

const j1939_spn_t *j1939_spn_find(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < TABLE_COUNT; i++)
    {
        if (strcmp(TABLE[i].name, name) == 0)
        {
            return &TABLE[i];
        }
    }

    return NULL;
}

j1939_raw_t j1939_spn_decode(const j1939_spn_t *spn, const uint8_t *data,
                             size_t len, double *value)
{
    if (spn == NULL || data == NULL || spn->bits == 0 || spn->bits > 32)
    {
        return J1939_RAW_SHORT;
    }

    size_t first = (size_t)spn->byte * 8u + spn->bit;
    size_t bytes = (first + spn->bits + 7u) / 8u; /* payload bytes needed */

    if (bytes > len)
    {
        return J1939_RAW_SHORT;
    }

    /* least significant byte first: gather up to 5 bytes, shift, mask */
    uint64_t acc = 0;

    for (size_t i = spn->byte; i < bytes; i++)
    {
        acc |= (uint64_t)data[i] << (8u * (i - spn->byte));
    }

    acc >>= spn->bit;

    uint32_t raw = (uint32_t)(acc & ((1ull << spn->bits) - 1u));
    j1939_raw_t cls = j1939_raw_class(raw, spn->bits);

    if (cls == J1939_RAW_VALID && value != NULL)
    {
        *value = (double)raw * spn->scale + spn->offset;
    }

    return cls;
}
