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
 * @file j1939_cache_core.h
 * @brief What the bus last said: the newest message of every (parameter
 *        group, source, destination) and who is on the bus (pure logic,
 *        host-testable; the caller supplies the time and the locking).
 *
 * A J1939 bus repeats itself: a reader does not want a stream, it wants
 * "engine speed, now". So the listener keeps one entry per (group, source,
 * destination) with the newest payload, when it came, how many came and how
 * far apart the last two were. Messages of up to 8 bytes live in the entry;
 * longer ones (transport protocol) borrow one of J1939_LONG_SLOTS buffers.
 *
 * Bounded, and loud about it: when the table is full a new key takes the
 * place of the entry that has been silent the longest, if that one has been
 * silent for J1939_EVICT_AGE_US (`evicted`); otherwise the message is not
 * kept (`full`). When every long buffer is taken the longest-silent holder
 * loses its payload (`lng_evicted`; its entry stays, with length 0).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "j1939_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define J1939_CACHE_SLOTS  512u /**< Hash slots (a power of two).              */
#define J1939_CACHE_MAX    384u /**< Entries kept: 3/4 of the slots.           */
#define J1939_LONG_SLOTS   12u  /**< Messages longer than a frame, at a time.  */
#define J1939_FRESH_US     5000000  /**< "Sends it now": heard this recently.  */
#define J1939_EVICT_AGE_US 30000000 /**< Silent this long: may make room.      */

#define J1939_PGN_FREE     0xFFFFFFFFu

typedef struct
{
    uint32_t pgn;       /**< J1939_PGN_FREE marks an empty slot.               */
    uint8_t  sa;
    uint8_t  da;
    int8_t   lng;       /**< Long buffer in use (len > 8), else -1.            */
    uint16_t len;       /**< Payload bytes; 0 = a long payload that had to go. */
    uint8_t  data[8];   /**< The payload when len <= 8.                        */
    uint32_t count;     /**< Messages received.                                */
    uint32_t period_us; /**< Between the last two (0 until the second).        */
    int64_t  rx_us;     /**< The last one.                                     */
} j1939_entry_t;

/** The store. About 42 KB: keep it static, in PSRAM. */
typedef struct
{
    j1939_entry_t e[J1939_CACHE_SLOTS];
    uint8_t  lng[J1939_LONG_SLOTS][J1939_MSG_MAX];
    int16_t  lng_owner[J1939_LONG_SLOTS]; /**< Slot of the holder, -1 free.    */
    uint16_t used;
    uint32_t seq;         /**< Messages stored since init: moves whenever any
                               entry changed.                                  */
    uint32_t full;        /**< Messages not kept: the table was full.          */
    uint32_t evicted;     /**< Entries dropped to make room for a new key.     */
    uint32_t lng_evicted; /**< Long payloads dropped for a newer long one.     */
} j1939_cache_t;

void j1939_cache_init(j1939_cache_t *c);

/**
 * @brief Store a message (a frame's data, or a reassembled long message).
 * @return Its entry; NULL when it could not be kept (counted in `full`).
 */
const j1939_entry_t *j1939_cache_put(j1939_cache_t *c, uint32_t pgn,
                                     uint8_t sa, uint8_t da,
                                     const uint8_t *data, uint16_t len,
                                     int64_t now_us);

/**
 * @brief The newest message of a group.
 *
 * @p sa and @p da may be J1939_ADDR_ANY. With more than one match: the
 * lowest source address among those heard within J1939_FRESH_US (the engine
 * is 0, and a pick must not flip between sources from one read to the next);
 * when none is that fresh, the lowest source of all, so a bus that falls
 * silent does not move the pick to whoever happened to speak last (an
 * autopid row would publish that stale message as news; bench 2026-10-03).
 * Entries without a payload are never returned.
 */
const j1939_entry_t *j1939_cache_get(const j1939_cache_t *c, uint32_t pgn,
                                     int sa, int da, int64_t now_us);

/** The payload of an entry (len bytes). */
const uint8_t *j1939_cache_payload(const j1939_cache_t *c,
                                   const j1939_entry_t *e);

/** Walk the store: the entry in @p slot (0 .. J1939_CACHE_SLOTS - 1), NULL
 *  when the slot is empty. */
const j1939_entry_t *j1939_cache_at(const j1939_cache_t *c, size_t slot);

/* ---- who is on the bus ------------------------------------------------------ */

/** One row per source address (about 5 KB). */
typedef struct
{
    uint32_t frames[256];  /**< Frames sent by this address.                   */
    int64_t  last_us[256]; /**< The last one.                                  */
    uint8_t  name[256][8]; /**< NAME of its address claim, as sent.            */
    uint8_t  named[32];    /**< One bit per address: a claim was seen.         */
} j1939_sources_t;

void j1939_sources_init(j1939_sources_t *s);

/** A frame from @p sa. */
void j1939_sources_note(j1939_sources_t *s, uint8_t sa, int64_t now_us);

/** An address claim from @p sa (8 data bytes). */
void j1939_sources_claim(j1939_sources_t *s, uint8_t sa, const uint8_t *name);

/** True when @p sa claimed its address; @p name (may be NULL) gets the NAME. */
bool j1939_sources_name(const j1939_sources_t *s, uint8_t sa, uint8_t *name);

/** Addresses heard at all. */
size_t j1939_sources_count(const j1939_sources_t *s);

#ifdef __cplusplus
}
#endif
