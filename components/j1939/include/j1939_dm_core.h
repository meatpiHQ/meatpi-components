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
 * @file j1939_dm_core.h
 * @brief J1939-73 diagnostic messages: the lamps and the trouble codes of a
 *        DM1 (active) or DM2 (previously active) payload (pure logic,
 *        host-testable).
 *
 * Payload: byte 1 the four lamps (2 bits each), byte 2 how they flash, then
 * one 4-byte record per trouble code:
 *
 *   SPN bits 7..0 | SPN bits 15..8 | SPN bits 18..16 (top 3 bits) + FMI
 *   (low 5 bits) | conversion method (top bit) + occurrence count (7 bits)
 *
 * "No trouble code" is one record of zeros. A message with one code fits a
 * frame (the last two bytes are FF); more codes travel by the transport
 * protocol.
 *
 * The record above is SPN conversion method version 4, which the conversion
 * method bit = 0 announces (every controller built to the standard since
 * 1996). With the bit set the SPN bits are laid out in one of three older
 * ways that the message does not tell apart: the record is decoded the same
 * way and flagged (j1939_dtc_t.cm), nothing is guessed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define J1939_DTC_TEXT_LEN 16 /**< "SPN524287-31" and the NUL fit.             */
#define J1939_OC_NA        127u /**< Occurrence count: not available.          */

/** A lamp's state as a diagnostic message reports it. */
typedef enum
{
    J1939_LAMP_OFF = 0,
    J1939_LAMP_ON = 1,
    J1939_LAMP_ERROR = 2,
    J1939_LAMP_NA = 3, /**< The sender has no such lamp.                       */
} j1939_lamp_t;

typedef struct
{
    uint8_t mil;   /**< Malfunction indicator lamp (emissions).                */
    uint8_t rsl;   /**< Red stop lamp: stop the vehicle.                       */
    uint8_t awl;   /**< Amber warning lamp: no need to stop.                   */
    uint8_t pl;    /**< Protect lamp: a non-electronic subsystem.              */
    uint8_t flash; /**< Byte 2 as sent (two bits per lamp, same order);
                        FF = no flashing information.                          */
} j1939_lamps_t;

typedef struct
{
    uint32_t spn; /**< Suspect parameter number, 19 bits.                      */
    uint8_t  fmi; /**< Failure mode identifier, 0..31.                         */
    uint8_t  oc;  /**< Occurrence count, J1939_OC_NA when not available.       */
    bool     cm;  /**< Conversion method bit set: an SPN layout older than
                       version 4, decoded as version 4 (see the file note).    */
} j1939_dtc_t;

/**
 * @brief Decode a DM1 / DM2 payload.
 *
 * @param p      Payload (a frame's 8 bytes or a reassembled message).
 * @param len    Its length.
 * @param lamps  Out (may be NULL). All "not available" when @p len < 2.
 * @param out    Out: the trouble codes, in message order (may be NULL with
 *               @p max 0 to count).
 * @param max    Room in @p out.
 * @return Trouble codes in the message, which may be more than @p max.
 */
size_t j1939_dm_parse(const uint8_t *p, size_t len, j1939_lamps_t *lamps,
                      j1939_dtc_t *out, size_t max);

/** The code as the firmware writes it everywhere: "SPN110-0". */
void j1939_dtc_text(uint32_t spn, uint8_t fmi, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
