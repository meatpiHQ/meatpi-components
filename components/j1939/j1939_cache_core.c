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
 * @file j1939_cache_core.c
 * @brief The newest message per (group, source, destination) and the table
 *        of sources. Pure: an open-addressing hash table with linear
 *        probing; removal shifts the cluster back, so there are no
 *        tombstones and a lookup stops at the first empty slot.
 */
#include "j1939_cache_core.h"

#include <string.h>

#define SLOT_MASK (J1939_CACHE_SLOTS - 1u)

static size_t slot_home(uint32_t pgn, uint8_t sa, uint8_t da)
{
    uint32_t h = pgn * 0x9E3779B1u;

    h ^= (((uint32_t)sa << 8) | da) * 0x85EBCA6Bu;
    h ^= h >> 15;
    return (size_t)(h & SLOT_MASK);
}

/** The slot holding the key, or the empty slot where it would go.
 *  @p found says which. The table is never full (J1939_CACHE_MAX). */
static size_t slot_find(const j1939_cache_t *c, uint32_t pgn, uint8_t sa,
                        uint8_t da, bool *found)
{
    size_t i = slot_home(pgn, sa, da);

    for (size_t n = 0; n < J1939_CACHE_SLOTS; n++)
    {
        const j1939_entry_t *e = &c->e[i];

        if (e->pgn == J1939_PGN_FREE)
        {
            break;
        }

        if (e->pgn == pgn && e->sa == sa && e->da == da)
        {
            *found = true;
            return i;
        }

        i = (i + 1u) & SLOT_MASK;
    }

    *found = false;
    return i;
}

/** Empty slot @p i and close the gap it leaves in its probe cluster. */
static void slot_remove(j1939_cache_t *c, size_t i)
{
    if (c->e[i].lng >= 0)
    {
        c->lng_owner[c->e[i].lng] = -1;
    }

    c->e[i].pgn = J1939_PGN_FREE;
    c->used--;

    size_t j = i;

    for (;;)
    {
        j = (j + 1u) & SLOT_MASK;

        if (c->e[j].pgn == J1939_PGN_FREE)
        {
            return;
        }

        size_t k = slot_home(c->e[j].pgn, c->e[j].sa, c->e[j].da);
        /* an entry whose home lies in (i, j] is still reachable where it is */
        bool stays = (i <= j) ? (k > i && k <= j) : (k > i || k <= j);

        if (stays)
        {
            continue;
        }

        c->e[i] = c->e[j];

        if (c->e[i].lng >= 0)
        {
            c->lng_owner[c->e[i].lng] = (int16_t)i;
        }

        c->e[j].pgn = J1939_PGN_FREE;
        i = j;
    }
}

/** The entry that has been silent the longest; -1 on an empty table. */
static int slot_oldest(const j1939_cache_t *c)
{
    int best = -1;

    for (size_t i = 0; i < J1939_CACHE_SLOTS; i++)
    {
        if (c->e[i].pgn != J1939_PGN_FREE &&
            (best < 0 || c->e[i].rx_us < c->e[best].rx_us))
        {
            best = (int)i;
        }
    }

    return best;
}

/** A long buffer for the entry in @p slot: its own, a free one, or the one
 *  of the holder that has been silent the longest. */
static int lng_take(j1939_cache_t *c, size_t slot)
{
    int pick = -1;

    if (c->e[slot].lng >= 0)
    {
        return c->e[slot].lng;
    }

    for (int i = 0; i < (int)J1939_LONG_SLOTS; i++)
    {
        if (c->lng_owner[i] < 0)
        {
            pick = i;
            break;
        }

        if (pick < 0 ||
            c->e[c->lng_owner[i]].rx_us < c->e[c->lng_owner[pick]].rx_us)
        {
            pick = i;
        }
    }

    if (c->lng_owner[pick] >= 0)
    {
        j1939_entry_t *loser = &c->e[c->lng_owner[pick]];

        loser->lng = -1;
        loser->len = 0;
        c->lng_evicted++;
    }

    c->lng_owner[pick] = (int16_t)slot;
    c->e[slot].lng = (int8_t)pick;
    return pick;
}

void j1939_cache_init(j1939_cache_t *c)
{
    memset(c, 0, sizeof(*c));

    for (size_t i = 0; i < J1939_CACHE_SLOTS; i++)
    {
        c->e[i].pgn = J1939_PGN_FREE;
        c->e[i].lng = -1;
    }

    for (size_t i = 0; i < J1939_LONG_SLOTS; i++)
    {
        c->lng_owner[i] = -1;
    }
}

const j1939_entry_t *j1939_cache_put(j1939_cache_t *c, uint32_t pgn,
                                     uint8_t sa, uint8_t da,
                                     const uint8_t *data, uint16_t len,
                                     int64_t now_us)
{
    bool found = false;
    size_t i;

    if (pgn == J1939_PGN_FREE || len > J1939_MSG_MAX ||
        (len > 0 && data == NULL))
    {
        return NULL;
    }

    i = slot_find(c, pgn, sa, da, &found);

    if (!found)
    {
        if (c->used >= J1939_CACHE_MAX)
        {
            int old = slot_oldest(c);

            if (old < 0 || now_us - c->e[old].rx_us < J1939_EVICT_AGE_US)
            {
                c->full++;
                return NULL;
            }

            slot_remove(c, (size_t)old);
            c->evicted++;
            i = slot_find(c, pgn, sa, da, &found); /* the table moved */
        }

        j1939_entry_t *n = &c->e[i];

        n->pgn = pgn;
        n->sa = sa;
        n->da = da;
        n->lng = -1;
        n->len = 0;
        n->count = 0;
        n->period_us = 0;
        n->rx_us = now_us;
        c->used++;
    }

    j1939_entry_t *e = &c->e[i];

    if (e->count > 0)
    {
        int64_t gap = now_us - e->rx_us;

        e->period_us = (gap < 0) ? 0
                       : (gap > (int64_t)UINT32_MAX) ? UINT32_MAX
                                                     : (uint32_t)gap;
    }

    e->count++;
    e->rx_us = now_us;

    if (len <= sizeof(e->data))
    {
        if (e->lng >= 0)
        {
            c->lng_owner[e->lng] = -1;
            e->lng = -1;
        }

        if (len > 0)
        {
            memcpy(e->data, data, len);
        }
    }
    else
    {
        memcpy(c->lng[lng_take(c, i)], data, len);
    }

    e->len = len;
    c->seq++;
    return e;
}

const j1939_entry_t *j1939_cache_get(const j1939_cache_t *c, uint32_t pgn,
                                     int sa, int da, int64_t now_us)
{
    if (sa >= 0 && da >= 0)
    {
        bool found = false;
        size_t i = slot_find(c, pgn, (uint8_t)sa, (uint8_t)da, &found);

        return (found && c->e[i].len > 0) ? &c->e[i] : NULL;
    }

    const j1939_entry_t *fresh = NULL;  /* lowest source heard recently */
    const j1939_entry_t *lowest = NULL; /* else: the lowest source, so the
                                           pick does not move when the bus
                                           falls silent (2026-10-03)     */

    for (size_t i = 0; i < J1939_CACHE_SLOTS; i++)
    {
        const j1939_entry_t *e = &c->e[i];

        if (e->pgn != pgn || e->len == 0 || (sa >= 0 && e->sa != sa) ||
            (da >= 0 && e->da != da))
        {
            continue;
        }

        if (lowest == NULL || e->sa < lowest->sa ||
            (e->sa == lowest->sa && e->rx_us > lowest->rx_us))
        {
            lowest = e;
        }

        if (now_us - e->rx_us > J1939_FRESH_US)
        {
            continue;
        }

        if (fresh == NULL || e->sa < fresh->sa ||
            (e->sa == fresh->sa && e->rx_us > fresh->rx_us))
        {
            fresh = e;
        }
    }

    return (fresh != NULL) ? fresh : lowest;
}

const uint8_t *j1939_cache_payload(const j1939_cache_t *c,
                                   const j1939_entry_t *e)
{
    if (e == NULL)
    {
        return NULL;
    }

    return (e->lng >= 0) ? c->lng[e->lng] : e->data;
}

const j1939_entry_t *j1939_cache_at(const j1939_cache_t *c, size_t slot)
{
    if (slot >= J1939_CACHE_SLOTS || c->e[slot].pgn == J1939_PGN_FREE)
    {
        return NULL;
    }

    return &c->e[slot];
}

/* ---- sources -------------------------------------------------------------------- */

void j1939_sources_init(j1939_sources_t *s)
{
    memset(s, 0, sizeof(*s));
}

void j1939_sources_note(j1939_sources_t *s, uint8_t sa, int64_t now_us)
{
    s->frames[sa]++;
    s->last_us[sa] = now_us;
}

void j1939_sources_claim(j1939_sources_t *s, uint8_t sa, const uint8_t *name)
{
    memcpy(s->name[sa], name, 8);
    s->named[sa >> 3] |= (uint8_t)(1u << (sa & 7u));
}

bool j1939_sources_name(const j1939_sources_t *s, uint8_t sa, uint8_t *name)
{
    if ((s->named[sa >> 3] & (1u << (sa & 7u))) == 0)
    {
        return false;
    }

    if (name != NULL)
    {
        memcpy(name, s->name[sa], 8);
    }

    return true;
}

size_t j1939_sources_count(const j1939_sources_t *s)
{
    size_t n = 0;

    for (size_t i = 0; i < 256; i++)
    {
        if (s->frames[i] != 0)
        {
            n++;
        }
    }

    return n;
}
