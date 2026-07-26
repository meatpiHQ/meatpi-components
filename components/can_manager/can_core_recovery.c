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
 * @file can_core_recovery.c
 * @brief Bus-off recovery restart policy (pure logic, host-testable).
 */
#include <string.h>

#include "can_core_recovery.h"

/* Wrap-safe "a >= b" for uint32 millisecond timestamps */
static bool time_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

void can_core_recovery_reset(can_core_recovery_t *r)
{
    if (r != NULL)
    {
        memset(r, 0, sizeof(*r));
    }
}

uint32_t can_core_recovery_on_bus_off(can_core_recovery_t *r,
                                        uint32_t now_ms)
{
    if (r == NULL)
    {
        return 0;
    }

    bool rapid = (r->off_count > 0)
                 && !time_reached(now_ms,
                                  r->last_off_ms
                                  + CAN_CORE_RECOVERY_STABLE_MS);

    if (!rapid)
    {
        r->backoff_ms = 0; /* isolated fault: restart as soon as recovered */
    }
    else if (r->backoff_ms == 0)
    {
        r->backoff_ms = CAN_CORE_RECOVERY_BACKOFF_FIRST_MS;
    }
    else if (r->backoff_ms < CAN_CORE_RECOVERY_BACKOFF_MAX_MS)
    {
        uint32_t next = r->backoff_ms * 2u;
        r->backoff_ms = (next < CAN_CORE_RECOVERY_BACKOFF_MAX_MS)
                        ? next
                        : CAN_CORE_RECOVERY_BACKOFF_MAX_MS;
    }

    r->off_count++;
    r->last_off_ms = now_ms;
    r->restart_pending = false; /* a new episode voids any armed restart */
    return r->backoff_ms;
}

void can_core_recovery_on_recovered(can_core_recovery_t *r,
                                      uint32_t now_ms)
{
    if (r == NULL || r->off_count == 0)
    {
        return; /* spurious RECOVERED without a preceding bus-off */
    }

    r->restart_pending = true;
    r->restart_due_ms  = now_ms + r->backoff_ms;
}

bool can_core_recovery_restart_due(can_core_recovery_t *r,
                                     uint32_t now_ms)
{
    if (r == NULL || !r->restart_pending
        || !time_reached(now_ms, r->restart_due_ms))
    {
        return false;
    }

    r->restart_pending = false;
    return true;
}

void can_core_recovery_retry(can_core_recovery_t *r,
                               uint32_t now_ms, uint32_t delay_ms)
{
    if (r == NULL)
    {
        return;
    }

    r->restart_pending = true;
    r->restart_due_ms  = now_ms + delay_ms;
}
