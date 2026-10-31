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
 * @file lwip_guard_private.h
 * @brief The pure part of the guard (host-tested): the refusal counter and
 *        the log rate limit.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** One E line per this many milliseconds while refusals go on. */
#define LWIP_GUARD_LOG_PERIOD_MS 10000u

typedef struct
{
    uint32_t refusals;      /**< every refusal since boot               */
    uint32_t last_log_ms;   /**< the clock of the last logged refusal   */
    bool     logged_once;   /**< the first refusal is always logged     */
} lwip_guard_state_t;

/**
 * Count a refusal at @p now_ms. Returns true when this one is to be
 * logged: the first, then one per LWIP_GUARD_LOG_PERIOD_MS (a wrapping
 * millisecond clock is fine).
 */
bool lwip_guard_note(lwip_guard_state_t *st, uint32_t now_ms);
