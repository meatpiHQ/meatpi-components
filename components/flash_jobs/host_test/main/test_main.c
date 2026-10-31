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
 * @file test_main.c
 * @brief Host tests for flash_jobs' pure core: where a call runs, the
 *        worker's creation and exit, the race between a publish and the
 *        idle timeout, the counters and the refusal log rate limit.
 *        Expected output: 10 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "flash_jobs_private.h"

static fj_state_t s_st;

void setUp(void)
{
    memset(&s_st, 0, sizeof(s_st));
}

void tearDown(void)
{
}

static void test_an_internal_stack_runs_in_line(void)
{
    TEST_ASSERT_EQUAL(FJ_INLINE, fj_route(true));
    TEST_ASSERT_EQUAL(FJ_MARSHAL, fj_route(false));
}

static void test_the_first_publish_asks_for_a_worker(void)
{
    TEST_ASSERT_TRUE(fj_publish(&s_st));
    TEST_ASSERT_TRUE(s_st.job_pending);
    fj_spawned(&s_st, true);
    TEST_ASSERT_TRUE(s_st.worker_alive);
    TEST_ASSERT_EQUAL_UINT32(1, s_st.spawns);
}

static void test_a_publish_with_a_live_worker_needs_no_spawn(void)
{
    (void)fj_publish(&s_st);
    fj_spawned(&s_st, true);
    TEST_ASSERT_EQUAL(FJ_WORKER_RUN, fj_worker_wake(&s_st, true));
    TEST_ASSERT_FALSE(fj_publish(&s_st));
    TEST_ASSERT_EQUAL_UINT32(1, s_st.spawns);
}

static void test_a_failed_spawn_withdraws_the_job(void)
{
    TEST_ASSERT_TRUE(fj_publish(&s_st));
    fj_spawned(&s_st, false);
    TEST_ASSERT_FALSE(s_st.job_pending);
    TEST_ASSERT_FALSE(s_st.worker_alive);
    TEST_ASSERT_EQUAL_UINT32(0, s_st.spawns);
}

static void test_the_worker_runs_a_pending_job_on_its_signal(void)
{
    (void)fj_publish(&s_st);
    fj_spawned(&s_st, true);
    TEST_ASSERT_EQUAL(FJ_WORKER_RUN, fj_worker_wake(&s_st, true));
    TEST_ASSERT_FALSE(s_st.job_pending);
    TEST_ASSERT_TRUE(s_st.worker_alive);
}

static void test_a_job_published_at_the_timeout_still_runs(void)
{
    /* the caller published and signalled a moment after the worker's wait
       ran out: the worker wakes with signalled = false and a pending job,
       and must run it (the caller is blocked on it) */
    (void)fj_publish(&s_st);
    fj_spawned(&s_st, true);
    TEST_ASSERT_EQUAL(FJ_WORKER_RUN, fj_worker_wake(&s_st, false));
    TEST_ASSERT_TRUE(s_st.worker_alive);
    /* the signal left behind wakes the worker once more: nothing to do */
    TEST_ASSERT_EQUAL(FJ_WORKER_WAIT, fj_worker_wake(&s_st, true));
    TEST_ASSERT_TRUE(s_st.worker_alive);
}

static void test_an_idle_period_ends_the_worker(void)
{
    (void)fj_publish(&s_st);
    fj_spawned(&s_st, true);
    (void)fj_worker_wake(&s_st, true);
    TEST_ASSERT_EQUAL(FJ_WORKER_EXIT, fj_worker_wake(&s_st, false));
    TEST_ASSERT_FALSE(s_st.worker_alive);
    /* the next caller creates the next worker */
    TEST_ASSERT_TRUE(fj_publish(&s_st));
}

static void test_the_exit_keeps_the_lowest_watermark(void)
{
    fj_worker_exit(&s_st, 0);
    TEST_ASSERT_EQUAL_UINT32(0, s_st.stack_hw_min);
    fj_worker_exit(&s_st, 2000);
    TEST_ASSERT_EQUAL_UINT32(2000, s_st.stack_hw_min);
    fj_worker_exit(&s_st, 2500);
    TEST_ASSERT_EQUAL_UINT32(2000, s_st.stack_hw_min);
    fj_worker_exit(&s_st, 1800);
    TEST_ASSERT_EQUAL_UINT32(1800, s_st.stack_hw_min);
    TEST_ASSERT_EQUAL_UINT32(4, s_st.exits);
}

static void test_the_counters_tell_the_two_paths_apart(void)
{
    fj_done(&s_st, FJ_INLINE);
    fj_done(&s_st, FJ_INLINE);
    fj_done(&s_st, FJ_MARSHAL);
    TEST_ASSERT_EQUAL_UINT32(2, s_st.inline_runs);
    TEST_ASSERT_EQUAL_UINT32(1, s_st.marshalled);
}

static void test_refusals_are_counted_and_logged_once_per_period(void)
{
    TEST_ASSERT_TRUE(fj_refused(&s_st, 100));
    TEST_ASSERT_FALSE(fj_refused(&s_st, 100 + FLASH_JOBS_LOG_PERIOD_MS - 1));
    TEST_ASSERT_TRUE(fj_refused(&s_st, 100 + FLASH_JOBS_LOG_PERIOD_MS));
    TEST_ASSERT_EQUAL_UINT32(3, s_st.refusals);
    /* a wrapping millisecond clock */
    s_st.last_log_ms = 0xFFFFFF00u;
    s_st.logged_once = true;
    TEST_ASSERT_FALSE(fj_refused(&s_st, 0xFFFFFFF0u));
    TEST_ASSERT_TRUE(fj_refused(&s_st, FLASH_JOBS_LOG_PERIOD_MS));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_internal_stack_runs_in_line);
    RUN_TEST(test_the_first_publish_asks_for_a_worker);
    RUN_TEST(test_a_publish_with_a_live_worker_needs_no_spawn);
    RUN_TEST(test_a_failed_spawn_withdraws_the_job);
    RUN_TEST(test_the_worker_runs_a_pending_job_on_its_signal);
    RUN_TEST(test_a_job_published_at_the_timeout_still_runs);
    RUN_TEST(test_an_idle_period_ends_the_worker);
    RUN_TEST(test_the_exit_keeps_the_lowest_watermark);
    RUN_TEST(test_the_counters_tell_the_two_paths_apart);
    RUN_TEST(test_refusals_are_counted_and_logged_once_per_period);
    UNITY_END();
}
