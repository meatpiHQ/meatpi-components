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
 * @file restart_tracker_brake_core.c
 * @brief The crash-loop brake's pure half: the count a boot keeps beside the
 *        crash notes and the verdict it reaches. No IDF dependencies:
 *        compiled as-is by the host unit tests.
 *
 * The rule, whole. A run that ends in a crash (the panic handler or a
 * watchdog) before it settled adds one to the streak; at
 * RESTART_TRACKER_BRAKE_STREAK the boot's verdict is PARK. A run that
 * settles ends the streak, and so does any reset that is not a crash. A park
 * that ends to try again (its timer, the button) keeps the streak: one more
 * quick crash parks at once. A crash inside the park gives PARK_BARE: park
 * again with less (the composition root leaves out what has a driver behind
 * it, the LED, and keeps the pin writes that put the board to sleep).
 */
#include "restart_tracker_private.h"

#include <stddef.h>
#include <string.h>

/* esp_reset_reason_t, the IDF 5+ numbering (restart_tracker_core.c) */
#define RT_RST_PANIC      4U
#define RT_RST_INT_WDT    5U
#define RT_RST_TASK_WDT   6U
#define RT_RST_WDT        7U
#define RT_RST_CPU_LOCKUP 15U

bool rt_reset_reason_is_crash(uint32_t reason)
{
    switch (reason)
    {
        case RT_RST_PANIC:
        case RT_RST_INT_WDT:
        case RT_RST_TASK_WDT:
        case RT_RST_WDT: /* the RTC watchdog: a boot or a panic handler that hung */
        case RT_RST_CPU_LOCKUP:
            return true;
        default:
            return false; /* power, brown-out, the EN pin, a restart call */
    }
}

uint32_t rt_brake_crc(const rt_brake_state_t *st)
{
    return rt_crc32_bytes((const uint8_t *)st, offsetof(rt_brake_state_t, crc));
}

bool rt_brake_valid(const rt_brake_state_t *st)
{
    return st->run_mode <= RESTART_TRACKER_BOOT_SAFE &&
           st->budget <= RT_REPORT_BUDGET &&
           rt_brake_crc(st) == st->crc;
}

void rt_brake_reset(rt_brake_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->budget = RT_REPORT_BUDGET;
    st->crc = rt_brake_crc(st);
}

static uint8_t bump(uint8_t value)
{
    return (value < UINT8_MAX) ? (uint8_t)(value + 1U) : value;
}

void rt_brake_boot(rt_brake_state_t *st, bool crash_reset, uint32_t retry_s,
                   restart_tracker_brake_t *out)
{
    uint8_t before = st->run_mode;
    bool settled = (st->run_flags & RT_BRAKE_RF_SETTLED) != 0U;
    bool retry = (st->run_flags & RT_BRAKE_RF_RETRY) != 0U;
    uint8_t verdict = RESTART_TRACKER_BOOT_NORMAL;

    if (crash_reset)
    {
        if (before == RESTART_TRACKER_BOOT_PARK ||
            before == RESTART_TRACKER_BOOT_PARK_BARE)
        {
            /* the park crashed: park again, with less */
            verdict = RESTART_TRACKER_BOOT_PARK_BARE;
        }
        else if (before == RESTART_TRACKER_BOOT_NORMAL && !settled)
        {
            st->streak = bump(st->streak);

            if (st->streak >= RESTART_TRACKER_BRAKE_STREAK)
            {
                verdict = RESTART_TRACKER_BOOT_PARK;
            }
        }
        else if (before == RESTART_TRACKER_BOOT_SAFE)
        {
            /* safe mode is the user's own recovery: nothing to brake */
            st->streak = 0;
            st->parks = 0;
        }

        /* else a settled run crashed: its streak ended when it settled,
           and one crash of a healthy run is not a loop */
    }
    else if (!retry)
    {
        /* a planned restart, a wake, the EN pin, a brown-out (a power-on
           never gets here: the store starts empty): the loop, if there was
           one, is over */
        st->streak = 0;
        st->parks = 0;
    }

    /* else a park ended to try again: the streak stands, this run is the
       one try */

    out->verdict = verdict;
    out->retry_after_s = 0;

    if (verdict != RESTART_TRACKER_BOOT_NORMAL)
    {
        st->parks = bump(st->parks);
        out->retry_after_s = (st->test_retry_s != 0U) ? st->test_retry_s
                                                      : retry_s;
        st->test_retry_s = 0; /* the bench's knob is for one park */
    }

    out->streak = st->streak;
    out->parks = st->parks;
    out->report_budget = st->budget;
    out->settled = false;

    /* the run that starts now */
    st->run_mode = RESTART_TRACKER_BOOT_NORMAL;
    st->run_flags = 0;
    st->crc = rt_brake_crc(st);
}

void rt_brake_settle(rt_brake_state_t *st)
{
    st->run_flags |= RT_BRAKE_RF_SETTLED;
    st->streak = 0;
    st->parks = 0;
    st->budget = RT_REPORT_BUDGET;
    st->crc = rt_brake_crc(st);
}

void rt_brake_set_mode(rt_brake_state_t *st, uint8_t mode)
{
    st->run_mode = (mode <= RESTART_TRACKER_BOOT_SAFE)
                       ? mode : (uint8_t)RESTART_TRACKER_BOOT_NORMAL;
    st->crc = rt_brake_crc(st);
}

void rt_brake_mark_retry(rt_brake_state_t *st)
{
    st->run_flags |= RT_BRAKE_RF_RETRY;
    st->crc = rt_brake_crc(st);
}

bool rt_brake_spend(rt_brake_state_t *st)
{
    if (st->budget == 0U)
    {
        return false;
    }

    st->budget--;
    st->crc = rt_brake_crc(st);
    return true;
}

void rt_brake_set_test_retry(rt_brake_state_t *st, uint16_t seconds)
{
    st->test_retry_s = seconds;
    st->crc = rt_brake_crc(st);
}

const char *restart_tracker_boot_mode_to_str(uint8_t mode)
{
    switch (mode)
    {
        case RESTART_TRACKER_BOOT_NORMAL:    return "normal";
        case RESTART_TRACKER_BOOT_PARK:      return "park";
        case RESTART_TRACKER_BOOT_PARK_BARE: return "park_bare";
        case RESTART_TRACKER_BOOT_SAFE:      return "safe";
        default:                             return "invalid";
    }
}
