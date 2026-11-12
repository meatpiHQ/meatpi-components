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
 * @file dev_status_manager_private.h
 * @brief Pure helpers shared with the host tests (dev_status_manager_fmt.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *dsm_bit_name(uint32_t bit);
size_t dsm_format_uptime(uint64_t uptime_us, char *buf, size_t buf_len);

/** One more occurrence of a fault code (dev_status_fault_count.c, pure).
 *  @p count goes up by one and stops at DEV_STATUS_FAULT_COUNT_MAX.
 *  Returns whether the record is to be written to the flash: a new code
 *  always, a known one at most once per boot (@p saved_this_boot) and never
 *  once its count was full before this occurrence. */
bool dsm_fault_count_step(uint32_t *count, bool is_new, bool saved_this_boot);

/** Load the latched fault codes from NVS (dev_status_faults.c);
 *  called once from dev_status_manager_init. */
void dsm_faults_load(void);

#ifdef __cplusplus
}
#endif
