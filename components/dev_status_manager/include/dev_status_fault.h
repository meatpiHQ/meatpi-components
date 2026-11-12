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
 * @file dev_status_fault.h
 * @brief A device fault code's record and the cap of its count: plain
 *        types, no IDF header, so the host tests read them too.
 *        dev_status_manager.h includes this file; the functions that raise,
 *        list and clear the codes are declared there.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_STATUS_FAULT_MAX 16

/** Where a fault's count stops (2026-11-12). A fault that recurs at every
 *  boot cost a write of the store per boot for good, and a larger number
 *  says nothing more. The occurrence that brings the count here is the
 *  last one written: behind it a recurrence changes the record in RAM only
 *  (its detail and last time, until the next restart) and costs the flash
 *  nothing. A count read at this value means "this many or more". A record
 *  stored with a larger count by an earlier build is kept as it is. */
#define DEV_STATUS_FAULT_COUNT_MAX 10

typedef struct
{
    char     code[24];   /* stable identifier, e.g. "cmdline_table_full" */
    char     detail[48]; /* latest human-readable context                */
    uint32_t count;      /* occurrences since the last clear, counted up
                            to DEV_STATUS_FAULT_COUNT_MAX                */
    uint32_t first_time; /* unix seconds (0 = clock not set yet)         */
    uint32_t last_time;
} dev_status_fault_t;

/** The count of @p f stands at its cap: read it as "this many or more". */
bool dev_status_manager_fault_capped(const dev_status_fault_t *f);

#ifdef __cplusplus
}
#endif
