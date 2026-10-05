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
 * @file restart_tracker_crash_stub.c
 * @brief The crash note on a target without its hooks (not Xtensa, or no
 *        RTC memory): nothing is recorded, the readers are told so. Built
 *        instead of restart_tracker_crash.c (CMakeLists.txt).
 */
#include "restart_tracker_private.h"

const rt_crash_note_t *rt_crash_boot_collect(uint32_t sequence, uint32_t slot,
                                             bool fresh_history)
{
    (void)sequence;
    (void)slot;
    (void)fresh_history;
    return NULL;
}

rt_brake_state_t *rt_crash_brake_state(void)
{
    return NULL; /* no store: no count, and the brake never parks */
}

esp_err_t restart_tracker_get_crash(uint32_t sequence,
                                    restart_tracker_crash_t *out)
{
    (void)sequence;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
