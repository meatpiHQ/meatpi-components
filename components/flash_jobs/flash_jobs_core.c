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
 * @file flash_jobs_core.c
 * @brief The worker's lifetime as pure functions (host-tested). The
 *        protocol they encode: callers are serialized by a lock; a caller
 *        publishes its job and wakes the worker, creating one first when
 *        none is alive; the worker runs every pending job it finds when it
 *        wakes, whether the wake-up was the caller's signal or the idle
 *        timeout, and leaves only when a whole idle period passed with
 *        nothing pending. A caller that arrives after the worker decided to
 *        leave sees no worker and creates the next one.
 */
#include "flash_jobs_private.h"

fj_route_t fj_route(bool on_internal_stack)
{
    return on_internal_stack ? FJ_INLINE : FJ_MARSHAL;
}

bool fj_publish(fj_state_t *st)
{
    st->job_pending = true;
    return !st->worker_alive;
}

void fj_spawned(fj_state_t *st, bool ok)
{
    if (ok)
    {
        st->worker_alive = true;
        st->spawns++;
        return;
    }

    st->job_pending = false;
}

fj_worker_step_t fj_worker_wake(fj_state_t *st, bool signalled)
{
    if (st->job_pending)
    {
        st->job_pending = false;
        return FJ_WORKER_RUN;
    }

    if (!signalled)
    {
        st->worker_alive = false;
        return FJ_WORKER_EXIT;
    }

    return FJ_WORKER_WAIT;
}

void fj_done(fj_state_t *st, fj_route_t how)
{
    if (how == FJ_INLINE)
    {
        st->inline_runs++;
    }
    else
    {
        st->marshalled++;
    }
}

void fj_worker_exit(fj_state_t *st, uint32_t stack_hw)
{
    st->exits++;

    if (stack_hw != 0 && (st->stack_hw_min == 0 || stack_hw < st->stack_hw_min))
    {
        st->stack_hw_min = stack_hw;
    }
}

bool fj_refused(fj_state_t *st, uint32_t now_ms)
{
    st->refusals++;

    /* the first refusal is logged; then one per period, on a clock that
       may wrap (unsigned difference) */
    if (!st->logged_once ||
        (uint32_t)(now_ms - st->last_log_ms) >= FLASH_JOBS_LOG_PERIOD_MS)
    {
        st->logged_once = true;
        st->last_log_ms = now_ms;
        return true;
    }

    return false;
}
