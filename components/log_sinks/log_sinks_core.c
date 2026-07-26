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
 * @file log_sinks_core.c
 * @brief Pure record ring + batcher + rotation planner (no IDF deps).
 */
#include "log_sinks_core.h"

#include <string.h>

#define LS_HDR 2u /* per-record length prefix */

void ls_ring_init(ls_ring_t *r, uint8_t *buf, uint32_t size)
{
    r->buf = buf;
    r->size = size;
    r->head = 0;
    r->tail = 0;
    r->used = 0;
    r->count = 0;
}

static void ring_write_bytes(ls_ring_t *r, const uint8_t *src, uint32_t len)
{
    uint32_t first = r->size - r->head;

    if (first > len)
    {
        first = len;
    }

    memcpy(r->buf + r->head, src, first);
    memcpy(r->buf, src + first, len - first);
    r->head = (r->head + len) % r->size;
}

static void ring_read_bytes(const ls_ring_t *r, uint32_t from, uint8_t *dst,
                            uint32_t len)
{
    uint32_t first = r->size - from;

    if (first > len)
    {
        first = len;
    }

    memcpy(dst, r->buf + from, first);
    memcpy(dst + first, r->buf, len - first);
}

static uint16_t peek_len(const ls_ring_t *r)
{
    uint8_t hdr[LS_HDR];

    ring_read_bytes(r, r->tail, hdr, LS_HDR);
    return (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
}

static void drop_oldest(ls_ring_t *r)
{
    uint16_t len = peek_len(r);

    r->tail = (r->tail + LS_HDR + len) % r->size;
    r->used -= LS_HDR + len;
    r->count--;
}

uint32_t ls_ring_push(ls_ring_t *r, const void *data, uint16_t len)
{
    uint32_t evicted = 0;

    if (r->size <= LS_HDR)
    {
        return 0;
    }

    if ((uint32_t)len + LS_HDR > r->size)
    {
        len = (uint16_t)(r->size - LS_HDR); /* truncate to what can fit */
    }

    while (r->used + LS_HDR + len > r->size)
    {
        drop_oldest(r);
        evicted++;
    }

    uint8_t hdr[LS_HDR] = { (uint8_t)(len & 0xff), (uint8_t)(len >> 8) };

    ring_write_bytes(r, hdr, LS_HDR);
    ring_write_bytes(r, data, len);
    r->used += LS_HDR + len;
    r->count++;
    return evicted;
}

uint16_t ls_ring_pop(ls_ring_t *r, void *out, uint16_t max)
{
    if (r->count == 0)
    {
        return 0;
    }

    uint16_t len = peek_len(r);
    uint16_t copy = (len < max) ? len : max;

    ring_read_bytes(r, (r->tail + LS_HDR) % r->size, out, copy);
    r->tail = (r->tail + LS_HDR + len) % r->size;
    r->used -= LS_HDR + len;
    r->count--;
    return copy;
}

uint32_t ls_ring_pop_batch(ls_ring_t *r, void *dst, uint32_t budget,
                           uint32_t *lines_out)
{
    uint8_t *out = dst;
    uint32_t written = 0;
    uint32_t lines = 0;

    while (r->count > 0)
    {
        uint16_t len = peek_len(r);

        if (written > 0 && written + len > budget)
        {
            break; /* next whole record doesn't fit — leave it queued */
        }

        uint32_t room = budget - written;
        uint16_t copy = (len < room) ? len : (uint16_t)room;

        written += ls_ring_pop(r, out + written, copy);
        lines++;
    }

    if (lines_out != NULL)
    {
        *lines_out = lines;
    }

    return written;
}

/* ---- rotation planner --------------------------------------------------------- */

uint32_t ls_rotate_next_epoch(uint32_t now, uint32_t newest_existing)
{
    return (now > newest_existing) ? now : newest_existing + 1;
}

int ls_rotate_victims(uint32_t *epochs, int n, int keep)
{
    if (keep < 1)
    {
        keep = 1;
    }

    if (n <= keep)
    {
        return 0;
    }

    /* insertion sort ascending — n is tiny (retention counts <= 16) */
    for (int i = 1; i < n; i++)
    {
        uint32_t v = epochs[i];
        int j = i - 1;

        while (j >= 0 && epochs[j] > v)
        {
            epochs[j + 1] = epochs[j];
            j--;
        }

        epochs[j + 1] = v;
    }

    return n - keep;
}
