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
 * @file obd_gate_core.c
 * @brief Pure gate state machine (host-tested — no RTOS, time injected).
 *        Single holder + a one-deep next-turn reservation for fairness
 *        (see the og_core_try contract in obd_gate_private.h).
 */
#include "obd_gate_private.h"

static bool reservation_live(const og_core_t *g, int64_t now_ms)
{
    return g->waiter != NULL && now_ms < g->waiter_deadline_ms;
}

bool og_core_try(og_core_t *g, const void *owner, int64_t now_ms,
                 uint32_t hold_ms)
{
    if (g == NULL || owner == NULL)
    {
        return false;
    }

    if (g->owner != NULL && g->owner != owner)
    {
        if (now_ms < g->deadline_ms)
        {
            /* held by someone else: register as (or stay) the waiter —
               first blocked owner takes the next turn */
            if (g->waiter == owner || !reservation_live(g, now_ms))
            {
                g->waiter = owner;
                g->waiter_deadline_ms = now_ms + OG_RESERVE_MS;
            }

            return false;
        }

        g->expiries++;   /* stale hold — reap it */
        g->owner = NULL; /* so the reservation applies below too */
    }

    /* the gate is free (or ours): honor a live reservation held by
       someone ELSE */
    if (g->owner == NULL && reservation_live(g, now_ms) &&
        g->waiter != owner)
    {
        return false;
    }

    if (g->waiter == owner)
    {
        g->waiter = NULL; /* collecting the reserved turn */
    }

    g->owner = owner;
    g->deadline_ms = now_ms + (int64_t)hold_ms;
    g->acquires++;
    return true;
}

void og_core_force(og_core_t *g, const void *owner, int64_t now_ms,
                   uint32_t hold_ms)
{
    if (g == NULL || owner == NULL)
    {
        return;
    }

    if (g->waiter == owner)
    {
        g->waiter = NULL;
    }

    g->owner = owner;
    g->deadline_ms = now_ms + (int64_t)hold_ms;
    g->steals++;
}

void og_core_release(og_core_t *g, const void *owner)
{
    if (g != NULL && g->owner == owner)
    {
        g->owner = NULL; /* a live waiter reservation stays in force */
    }
}
