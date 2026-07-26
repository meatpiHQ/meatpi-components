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
 * @file log_sinks_core.h
 * @brief The pure core (host-tested, no IDF deps): a byte ring of length-
 *        prefixed line records with drop-oldest eviction, a batcher that
 *        joins whole records up to a byte budget, and the file-rotation
 *        planner. Locking is the caller's job (log_sinks.c wraps every
 *        call in the component spinlock).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Byte ring of records, each stored as [u16 len][len bytes]. */
typedef struct
{
    uint8_t *buf;
    uint32_t size;
    uint32_t head;  /* next write offset               */
    uint32_t tail;  /* oldest record offset            */
    uint32_t used;  /* bytes occupied (incl. headers)  */
    uint32_t count; /* whole records in the ring       */
} ls_ring_t;

void ls_ring_init(ls_ring_t *r, uint8_t *buf, uint32_t size);

/**
 * Append one record, evicting oldest records until it fits. A record
 * longer than the ring can hold is truncated to fit. Returns the number
 * of records evicted to make room.
 */
uint32_t ls_ring_push(ls_ring_t *r, const void *data, uint16_t len);

/**
 * Pop the oldest record into @p out (up to @p max bytes; longer records
 * are truncated, the remainder discarded). Returns the copied length,
 * 0 when the ring is empty.
 */
uint16_t ls_ring_pop(ls_ring_t *r, void *out, uint16_t max);

/**
 * Pop as many WHOLE records as fit into @p dst within @p budget bytes,
 * concatenated. Records themselves longer than the budget are popped and
 * truncated to the budget (never silently stuck). @p lines_out (nullable)
 * gets the record count. Returns the byte count written.
 */
uint32_t ls_ring_pop_batch(ls_ring_t *r, void *dst, uint32_t budget,
                           uint32_t *lines_out);

static inline uint32_t ls_ring_count(const ls_ring_t *r)
{
    return r->count;
}

static inline uint32_t ls_ring_used(const ls_ring_t *r)
{
    return r->used;
}

/* ---- file rotation planner --------------------------------------------------- */

/**
 * Epoch for the NEXT log file: strictly newer than @p newest_existing even
 * when the wall clock rolled back (the data_logger monotonic-name rule).
 */
uint32_t ls_rotate_next_epoch(uint32_t now, uint32_t newest_existing);

/**
 * Retention: given the existing file epochs (any order), sort ascending in
 * place and return how many of the OLDEST must be deleted so that at most
 * @p keep remain. Returns 0 when nothing to prune (or keep <= 0 quirks are
 * clamped to keeping 1).
 */
int ls_rotate_victims(uint32_t *epochs, int n, int keep);

#ifdef __cplusplus
}
#endif
