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
 * @file can_frame_wire.h
 * @brief The bridge-internal CAN-frame chunk format — one CAN frame packed
 *        into a bridge_chunk_t payload. Shared, dependency-free, header-only:
 *        the `can` bridge endpoint and every CAN translator (slcan/gvret/
 *        realdash) use exactly this one definition so there is no drift.
 *
 * Layout (little-endian id; see bridge_manager/DESIGN_translators.md §2):
 *   off 0  size 4  identifier (uint32 LE, 11- or 29-bit)
 *   off 4  size 1  flags: bit0 = extended, bit1 = RTR (other bits reserved 0)
 *   off 5  size 1  dlc (0..8)
 *   off 6  size N  data[dlc]   (absent for RTR / dlc 0)
 *   total = 6 + dlc  (min 6, max 14)
 *
 * One chunk = one frame (≤14 B ≪ 128 B chunk), so the CAN side never needs
 * reassembly. This format is bridge-internal and never leaves the device.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "can_core.h" /* can_core_frame_t {id, ext, rtr, dlc, data[8]} */

#define CAN_WIRE_HDR      6
#define CAN_WIRE_MAX      (CAN_WIRE_HDR + 8) /* 14 */
#define CAN_WIRE_FLAG_EXT 0x01u
#define CAN_WIRE_FLAG_RTR 0x02u

/** Pack @p f into @p out (must hold >= CAN_WIRE_MAX). Returns bytes written. */
static inline size_t can_wire_encode(const can_core_frame_t *f, uint8_t *out)
{
    uint8_t dlc = (f->dlc > 8) ? 8 : f->dlc;

    out[0] = (uint8_t)(f->id);
    out[1] = (uint8_t)(f->id >> 8);
    out[2] = (uint8_t)(f->id >> 16);
    out[3] = (uint8_t)(f->id >> 24);
    out[4] = (uint8_t)((f->ext ? CAN_WIRE_FLAG_EXT : 0) |
                       (f->rtr ? CAN_WIRE_FLAG_RTR : 0));
    out[5] = dlc;

    if (!f->rtr && dlc)
    {
        memcpy(&out[CAN_WIRE_HDR], f->data, dlc);
    }

    return (size_t)(CAN_WIRE_HDR + (f->rtr ? 0 : dlc));
}

/** Unpack @p in/@p len into @p f. Returns false on any malformed input. */
static inline bool can_wire_decode(const uint8_t *in, size_t len,
                                   can_core_frame_t *f)
{
    if (len < CAN_WIRE_HDR)
    {
        return false;
    }

    uint8_t flags = in[4];
    uint8_t dlc = in[5];

    if (dlc > 8)
    {
        return false;
    }

    f->id = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
            ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    f->ext = (flags & CAN_WIRE_FLAG_EXT) != 0;
    f->rtr = (flags & CAN_WIRE_FLAG_RTR) != 0;
    f->dlc = dlc;

    memset(f->data, 0, sizeof(f->data));

    if (!f->rtr && dlc)
    {
        if (len < (size_t)(CAN_WIRE_HDR + dlc))
        {
            return false;
        }
        memcpy(f->data, &in[CAN_WIRE_HDR], dlc);
    }

    return true;
}

/** Decode the frame at the START of a possibly-COALESCED buffer (a
 *  coalesced chunk is a plain concatenation of wire frames — the `can`
 *  endpoint pump packs several per bridge chunk since 2026-07-18).
 *  Returns bytes consumed, 0 on short/malformed. Consumers loop:
 *      size_t off = 0, n;
 *      while ((n = can_wire_decode_next(in + off, len - off, &f)) > 0)
 *      { ...emit f...; off += n; }
 */
static inline size_t can_wire_decode_next(const uint8_t *in, size_t len,
                                          can_core_frame_t *f)
{
    if (len == 0 || !can_wire_decode(in, len, f))
    {
        return 0;
    }

    return (size_t)(CAN_WIRE_HDR + (f->rtr ? 0 : f->dlc));
}
