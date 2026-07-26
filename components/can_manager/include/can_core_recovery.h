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
 * @file can_core_recovery.h
 * @brief Bus-off recovery restart policy (pure logic, host-testable).
 *
 * The TWAI controller enters BUS_OFF at TEC=256 (e.g. a shorted bus or
 * a baud-mismatch collision storm while transmitting). Recovery itself
 * (128 x 11 recessive bits) is initiated immediately — it only completes
 * once the bus is electrically sane again. This module decides how long
 * to wait AFTER recovery completes before restarting the driver, so a
 * still-faulty bus can't thrash through off/recover/off cycles:
 *
 *   first bus-off (or >=60 s since the last one)  -> restart immediately
 *   re-offense within 60 s                        -> 1 s, 2 s, 4 s ... 30 s cap
 *
 * All times are uint32 milliseconds; comparisons are wrap-safe.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_CORE_RECOVERY_BACKOFF_FIRST_MS  1000u
#define CAN_CORE_RECOVERY_BACKOFF_MAX_MS   30000u
#define CAN_CORE_RECOVERY_STABLE_MS        60000u

typedef struct
{
    uint32_t off_count;       /**< Bus-off events since driver start.       */
    uint32_t backoff_ms;      /**< Restart delay chosen for this episode.   */
    uint32_t last_off_ms;     /**< Timestamp of the last bus-off.           */
    bool     restart_pending; /**< Recovered; waiting out the backoff.      */
    uint32_t restart_due_ms;  /**< When to restart (valid while pending).   */
} can_core_recovery_t;

/** Clear all state (driver installed / reconfigured). */
void can_core_recovery_reset(can_core_recovery_t *r);

/**
 * @brief Record a bus-off event and pick the restart backoff.
 * @return The backoff (ms) that will apply once recovery completes.
 */
uint32_t can_core_recovery_on_bus_off(can_core_recovery_t *r,
                                        uint32_t now_ms);

/** Record that hardware recovery completed; arms the delayed restart. */
void can_core_recovery_on_recovered(can_core_recovery_t *r,
                                      uint32_t now_ms);

/**
 * @brief Poll for the restart moment.
 * @return true exactly once, when a pending restart's backoff has
 *         elapsed (clears the pending flag).
 */
bool can_core_recovery_restart_due(can_core_recovery_t *r,
                                     uint32_t now_ms);

/** Re-arm a pending restart (twai_start failed; try again later). */
void can_core_recovery_retry(can_core_recovery_t *r,
                               uint32_t now_ms, uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
