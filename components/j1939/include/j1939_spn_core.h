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
 * @file j1939_spn_core.h
 * @brief J1939-71 values: the built-in table of well-known suspect parameter
 *        numbers (SPNs) and their decode, including "the sender has no such
 *        value" (pure logic, host-testable).
 *
 * A value sits in its parameter group at a fixed byte and bit, least
 * significant byte first, and is raw * scale + offset. The top of the raw
 * range is not data (J1939-71, table "transmitted signal ranges"); for one
 * byte, and for the top byte of longer values:
 *
 *   00..FA  valid            FB      parameter specific indicator
 *   FC, FD  reserved         FE      error (the sender cannot measure it)
 *   FF      not available (the sender has no such parameter)
 *
 * The table is small on purpose: the values almost every heavy vehicle
 * broadcasts. Everything else comes from a DBC file or a custom row. Names
 * are the SPN labels, and none equals a name of autopid's OBD table (an OBD
 * and a J1939 value of the same vehicle may both be configured).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a raw value is. */
typedef enum
{
    J1939_RAW_VALID = 0,
    J1939_RAW_SPECIFIC,      /**< Parameter specific indicator (FB).           */
    J1939_RAW_RESERVED,      /**< FC, FD.                                      */
    J1939_RAW_ERROR,         /**< FE: the sender cannot measure it right now.  */
    J1939_RAW_NOT_AVAILABLE, /**< FF: the sender has no such parameter.        */
    J1939_RAW_SHORT,         /**< The message is too short to hold it.         */
} j1939_raw_t;

/** One value of the built-in table. */
typedef struct
{
    uint32_t    spn;
    uint32_t    pgn;
    const char *name;      /**< Parameter name (the SPN label).                */
    const char *unit;      /**< In the words of autopid's OBD table.           */
    const char *dev_class; /**< Home Assistant device class, or "none".        */
    uint8_t     byte;      /**< First data byte, from 0.                       */
    uint8_t     bit;       /**< First bit in that byte, 0 = least significant. */
    uint8_t     bits;      /**< Length, 1..32.                                 */
    double      scale;
    double      offset;
    double      min;       /**< Operational range: the largest valid raw value */
    double      max;       /**< decodes to max.                                */
} j1939_spn_t;

/** What a raw value of @p bits bits is (never J1939_RAW_SHORT). */
j1939_raw_t j1939_raw_class(uint32_t raw, uint8_t bits);

/** "valid", "specific", "reserved", "error", "na" or "short". Never NULL. */
const char *j1939_raw_name(j1939_raw_t r);

/** The built-in table. */
const j1939_spn_t *j1939_spn_table(size_t *count);

/** A table entry by its name; NULL when there is none. */
const j1939_spn_t *j1939_spn_find(const char *name);

/**
 * @brief Decode one value from a parameter group's payload.
 *
 * @param spn    The value's place and scaling.
 * @param data   Payload of its parameter group.
 * @param len    Payload length.
 * @param value  Out: the value in engineering units, written only for
 *               J1939_RAW_VALID (may be NULL).
 * @return What the raw value is; only J1939_RAW_VALID carries a value.
 */
j1939_raw_t j1939_spn_decode(const j1939_spn_t *spn, const uint8_t *data,
                             size_t len, double *value);

#ifdef __cplusplus
}
#endif
