/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file lwip_guard_core.c
 * @brief The pure part of the guard: no RTOS, no lwIP, host-tested.
 */
#include "lwip_guard_private.h"

bool lwip_guard_note(lwip_guard_state_t *st, uint32_t now_ms)
{
    st->refusals++;

    if (!st->logged_once)
    {
        st->logged_once = true;
        st->last_log_ms = now_ms;
        return true;
    }

    if ((uint32_t)(now_ms - st->last_log_ms) >= LWIP_GUARD_LOG_PERIOD_MS)
    {
        st->last_log_ms = now_ms;
        return true;
    }

    return false;
}
