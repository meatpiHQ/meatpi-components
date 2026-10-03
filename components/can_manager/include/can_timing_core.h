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
 * @file can_timing_core.h
 * @brief The bit timing of the native CAN node (pure logic, host-testable).
 *
 * One shape for every bitrate: 20 time quanta per bit, sample point 80 %,
 * synchronisation jump width 3 quanta. It is the released WiCAN firmware's
 * timing table (wican-fw main/can.c: brp, 15, 4, sjw 3 for every rate but
 * 25 and 800 kbit/s), in the field since 2022.
 *
 * Why not leave it to the driver: below 500 kbit/s esp_driver_twai aims at
 * 87.5 %, which at 250 kbit/s comes out as 85 % with 3 quanta after the
 * sample point and a jump width of 1. Bench 2026-10-03 (PCAN + simulator,
 * 250 kbit/s, 200 frames/s): the node then read about one frame in 1500 as
 * a stuff or form error. Listen-only that is a counter; in normal mode each
 * one is an error flag that destroys the frame on the bus (8 in 60 s, none
 * in 90 s with the timing below, none at 500 kbit/s either way). The driver
 * also cannot make 83.3 or 33.3 kbit/s from the rounded numbers a setting
 * holds.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_TIMING_QUANTA 20u /**< Per bit: 1 sync + TSEG1 + TSEG2.          */
#define CAN_TIMING_TSEG1  15u /**< Quanta before the sample point (80 %).    */
#define CAN_TIMING_TSEG2  4u  /**< Quanta after it.                          */
#define CAN_TIMING_SJW    3u  /**< Resynchronisation jump width.             */

#define CAN_TIMING_BRP_MIN 2u     /**< The controller's prescaler: even,     */
#define CAN_TIMING_BRP_MAX 16384u /**< within these limits.                  */

typedef struct
{
    uint32_t brp;     /**< Prescaler: one quantum = brp / source clock.       */
    uint8_t  tseg1;
    uint8_t  tseg2;
    uint8_t  sjw;
    uint32_t bitrate; /**< bit/s these values make from the source clock.     */
} can_timing_t;

/**
 * @brief The bitrate a setting value stands for, in bit/s.
 *
 * The settings hold kbit/s as integers; three of the classic rates are not
 * integers: 33 = 33.333 (single-wire CAN), 83 = 83.333 and 95 = 95.238.
 */
uint32_t can_timing_nominal_bps(uint16_t kbps);

/**
 * @brief The timing for @p kbps from a source clock of @p src_hz.
 *
 * @return false when the clock cannot make the bitrate in this shape (the
 *         prescaler would be odd or out of range, or the result more than
 *         0.5 % off): the caller lets the driver calculate, with the sample
 *         point asked for. @p out is untouched then.
 */
bool can_timing_for(uint32_t src_hz, uint16_t kbps, can_timing_t *out);

#ifdef __cplusplus
}
#endif
