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
 * @file test_chip_watch.c
 * @brief The OBD chip through the naps (sleep_manager_chip.c): which
 *        reading of READY is a fault code and which is not (2026-11-12).
 */
#include "unity.h"

#include "sleep_manager_private.h"

static void test_chip_asleep_all_night_flags_nothing(void)
{
    sm_chip_watch_t w;

    sm_chip_watch_init(&w);

    for (int nap = 0; nap < 20000; nap++) /* eleven hours of 2 s naps */
    {
        TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    }

    TEST_ASSERT_EQUAL_UINT32(0, w.wakes);
}

static void test_chip_awake_at_the_first_nap_is_put_back_without_a_fault(void)
{
    /* the entry's sleep word was refused (an app left a monitor open): the
       first nap finds the chip awake. It never slept, so it did not wake. */
    sm_chip_watch_t w;

    sm_chip_watch_init(&w);
    TEST_ASSERT_EQUAL(SM_CHIP_RESLEEP, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL(SM_CHIP_RESLEEP, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    TEST_ASSERT_EQUAL_UINT32(0, w.wakes);
}

static void test_chip_that_wakes_after_sleeping_is_a_fault_once_per_wake(void)
{
    sm_chip_watch_t w;

    sm_chip_watch_init(&w);
    TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    /* THE case: it slept, the sleep pin is low, and it is awake */
    TEST_ASSERT_EQUAL(SM_CHIP_WOKE, sm_chip_watch_feed(&w, true));
    /* the re-sleep did not take at once: the same wake, not a second fault */
    TEST_ASSERT_EQUAL(SM_CHIP_RESLEEP, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    TEST_ASSERT_EQUAL_UINT32(1, w.wakes);
    /* an hour later it wakes again: a second one */
    TEST_ASSERT_EQUAL(SM_CHIP_WOKE, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    TEST_ASSERT_EQUAL_UINT32(2, w.wakes);
}

static void test_chip_that_will_not_sleep_gives_up_behind_six_resleeps(void)
{
    sm_chip_watch_t w;

    /* never asleep: six re-sleeps, then the give-up, and it stays there */
    sm_chip_watch_init(&w);

    for (int i = 0; i < SM_RESLEEP_MAX; i++)
    {
        TEST_ASSERT_EQUAL(SM_CHIP_RESLEEP, sm_chip_watch_feed(&w, true));
        TEST_ASSERT_EQUAL_UINT8(i + 1, w.in_a_row);
    }

    TEST_ASSERT_EQUAL(SM_CHIP_GIVE_UP, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL(SM_CHIP_GIVE_UP, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL_UINT32(0, w.wakes);

    /* it slept, woke and cannot be put back: the wake's fault first, then
       the give-up, at the same nap count as before */
    sm_chip_watch_init(&w);
    TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    TEST_ASSERT_EQUAL(SM_CHIP_WOKE, sm_chip_watch_feed(&w, true));

    for (int i = 1; i < SM_RESLEEP_MAX; i++)
    {
        TEST_ASSERT_EQUAL(SM_CHIP_RESLEEP, sm_chip_watch_feed(&w, true));
    }

    TEST_ASSERT_EQUAL(SM_CHIP_GIVE_UP, sm_chip_watch_feed(&w, true));
    TEST_ASSERT_EQUAL_UINT32(1, w.wakes);
}

static void test_chip_asleep_between_resleeps_starts_the_count_again(void)
{
    /* five failed re-sleeps, one nap asleep, five more: no give-up (the
       count is of re-sleeps IN A ROW, as before the fault codes) */
    sm_chip_watch_t w;

    sm_chip_watch_init(&w);

    for (int round = 0; round < 2; round++)
    {
        for (int i = 0; i < SM_RESLEEP_MAX - 1; i++)
        {
            TEST_ASSERT_NOT_EQUAL(SM_CHIP_GIVE_UP, sm_chip_watch_feed(&w, true));
        }

        TEST_ASSERT_EQUAL(SM_CHIP_ASLEEP, sm_chip_watch_feed(&w, false));
    }
}

static void test_chip_fault_codes_fit_the_fault_store(void)
{
    /* dev_status_fault_t.code is char[24] */
    TEST_ASSERT_LESS_THAN(24, sizeof(SM_FAULT_CHIP_WOKE));
    TEST_ASSERT_LESS_THAN(24, sizeof(SM_FAULT_CHIP_NO_SLEEP));
    TEST_ASSERT_EQUAL_STRING("obd_chip_woke", SM_FAULT_CHIP_WOKE);
    TEST_ASSERT_EQUAL_STRING("obd_chip_no_sleep", SM_FAULT_CHIP_NO_SLEEP);
}

void sm_chip_watch_tests(void)
{
    RUN_TEST(test_chip_asleep_all_night_flags_nothing);
    RUN_TEST(test_chip_awake_at_the_first_nap_is_put_back_without_a_fault);
    RUN_TEST(test_chip_that_wakes_after_sleeping_is_a_fault_once_per_wake);
    RUN_TEST(test_chip_that_will_not_sleep_gives_up_behind_six_resleeps);
    RUN_TEST(test_chip_asleep_between_resleeps_starts_the_count_again);
    RUN_TEST(test_chip_fault_codes_fit_the_fault_store);
}
