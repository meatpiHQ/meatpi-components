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
 * @file dev_status_fault_count.c
 * @brief Pure: one more occurrence of a fault code, its count and whether
 *        the record is worth a write of the flash. Compiled as it is by the
 *        host unit tests.
 *
 * A fault that recurs at every boot used to cost one write of the fault
 * store per boot for as long as the device lived (measured 2026-11-12: 184
 * bytes in 10 flash writes, a sector erase about every 21 boots). Its count
 * tells nothing more once it is large, so it stops at
 * DEV_STATUS_FAULT_COUNT_MAX, and with it the writes (Ali, 2026-11-12: "cap
 * recurring faults at a certain number, there is no point to keep
 * incrementing").
 */
#include "dev_status_fault.h"
#include "dev_status_manager_private.h"

bool dsm_fault_count_step(uint32_t *count, bool is_new, bool saved_this_boot)
{
    /* judged on the count BEFORE this occurrence: the occurrence that
       brings the count to the cap is still written, so the store says
       "the cap" and not "one short of it" */
    const bool full = (*count >= DEV_STATUS_FAULT_COUNT_MAX);

    if (!full)
    {
        (*count)++;
    }

    /* a new code is always written. A known one at most once per boot, and
       never again once its count is full */
    return is_new || (!saved_this_boot && !full);
}

bool dev_status_manager_fault_capped(const dev_status_fault_t *f)
{
    return f != NULL && f->count >= DEV_STATUS_FAULT_COUNT_MAX;
}
