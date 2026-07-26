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
 * @file battery_monitor_policy.c
 * @brief PURE threshold-watch state machine — hysteresis pair + hold
 *        debounce, injected time/voltage, no ADC, no RTOS.
 */
#include "battery_monitor_private.h"

bool bm_watch_init(bm_watch_state_t *w, float below_v, float above_v,
                   uint32_t hold_ms, uint32_t now_ms)
{
    if (above_v < below_v)
    {
        return false;
    }

    w->below_v = below_v;
    w->above_v = above_v;
    w->hold_ms = hold_ms;
    w->side = BM_SIDE_UNKNOWN;
    w->pending = BM_SIDE_UNKNOWN;
    w->pending_since = now_ms;
    return true;
}

int bm_watch_eval(bm_watch_state_t *w, float voltage, uint32_t now_ms)
{
    int seen = (voltage < w->below_v) ? BM_SIDE_BELOW :
               (voltage >= w->above_v) ? BM_SIDE_ABOVE :
               BM_SIDE_UNKNOWN; /* hysteresis band: no side */

    if (seen == BM_SIDE_UNKNOWN || seen == w->side)
    {
        w->pending = BM_SIDE_UNKNOWN; /* debounce broken / nothing new */
        return BM_EVAL_NONE;
    }

    if (seen != w->pending)
    {
        w->pending = seen;
        w->pending_since = now_ms;
    }

    if ((uint32_t)(now_ms - w->pending_since) >= w->hold_ms)
    {
        w->side = seen;
        w->pending = BM_SIDE_UNKNOWN;
        return (seen == BM_SIDE_BELOW) ? BM_EVAL_BELOW : BM_EVAL_ABOVE;
    }

    return BM_EVAL_NONE;
}
