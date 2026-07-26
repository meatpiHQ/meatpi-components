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
 * @file imu_manager_policy.c
 * @brief PURE activity state machine — motion events + injected time in,
 *        stationary/active out. No I2C, no RTOS; host-testable.
 */
#include "imu_manager_private.h"

#define IMU_STATE_STATIONARY 0
#define IMU_STATE_ACTIVE     1

void imu_policy_init(imu_policy_t *p, uint32_t stationary_after_ms,
                     uint32_t wom_throttle_ms, uint32_t now_ms)
{
    p->state = IMU_STATE_STATIONARY;
    p->last_motion_ms = now_ms;
    p->stationary_after_ms = stationary_after_ms;
    p->wom_throttle_ms = wom_throttle_ms;
    p->last_wom_pub_ms = now_ms;
    p->wom_pub_primed = false;
}

int imu_policy_on_motion(imu_policy_t *p, uint32_t now_ms)
{
    p->last_motion_ms = now_ms;
    p->state = IMU_STATE_ACTIVE;
    return p->state;
}

int imu_policy_on_tick(imu_policy_t *p, uint32_t now_ms)
{
    if (p->state == IMU_STATE_ACTIVE &&
        (uint32_t)(now_ms - p->last_motion_ms) >= p->stationary_after_ms)
    {
        p->state = IMU_STATE_STATIONARY;
    }

    return p->state;
}

bool imu_policy_wom_gate(imu_policy_t *p, uint32_t now_ms)
{
    if (p->wom_pub_primed &&
        (uint32_t)(now_ms - p->last_wom_pub_ms) < p->wom_throttle_ms)
    {
        return false;
    }

    p->wom_pub_primed = true;
    p->last_wom_pub_ms = now_ms;
    return true;
}
