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
 * @file can_timing_core.c
 * @brief The bit timing of the native CAN node (pure logic, host-testable).
 *        See can_timing_core.h.
 */
#include <stddef.h>

#include "can_timing_core.h"

uint32_t can_timing_nominal_bps(uint16_t kbps)
{
    switch (kbps)
    {
    case 33:
        return 33333u;

    case 83:
        return 83333u;

    case 95:
        return 95238u;

    default:
        return (uint32_t)kbps * 1000u;
    }
}

bool can_timing_for(uint32_t src_hz, uint16_t kbps, can_timing_t *out)
{
    uint32_t bps = can_timing_nominal_bps(kbps);

    if (out == NULL || src_hz == 0 || bps == 0)
    {
        return false;
    }

    uint64_t per_bit = (uint64_t)bps * CAN_TIMING_QUANTA;
    uint64_t brp = (src_hz + per_bit / 2) / per_bit; /* nearest */

    if (brp < CAN_TIMING_BRP_MIN || brp > CAN_TIMING_BRP_MAX ||
        (brp & 1u) != 0)
    {
        return false;
    }

    uint32_t actual = (uint32_t)(src_hz / (brp * CAN_TIMING_QUANTA));
    uint32_t off = (actual > bps) ? actual - bps : bps - actual;

    if ((uint64_t)off * 1000u > (uint64_t)bps * 5u) /* more than 0.5 % */
    {
        return false;
    }

    out->brp = (uint32_t)brp;
    out->tseg1 = CAN_TIMING_TSEG1;
    out->tseg2 = CAN_TIMING_TSEG2;
    out->sjw = CAN_TIMING_SJW;
    out->bitrate = actual;
    return true;
}
