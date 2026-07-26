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

/** @file test_main.c
 *  @brief Host tests for the log_sinks pure core: the record ring
 *         (drop-oldest, wrap, truncation), the batcher (whole-record
 *         budget), and the rotation planner (monotonic epochs,
 *         retention victims).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "log_sinks_core.h"

static ls_ring_t s_r;
static uint8_t s_buf[64];
static char s_out[128];

void setUp(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    ls_ring_init(&s_r, s_buf, sizeof(s_buf));
}

void tearDown(void)
{
}

static void push_str(const char *s)
{
    ls_ring_push(&s_r, s, (uint16_t)strlen(s));
}

static uint16_t pop_str(void)
{
    memset(s_out, 0, sizeof(s_out));
    return ls_ring_pop(&s_r, s_out, sizeof(s_out) - 1);
}

static void test_push_pop_roundtrip(void)
{
    push_str("hello\n");
    TEST_ASSERT_EQUAL_UINT32(1, ls_ring_count(&s_r));
    TEST_ASSERT_EQUAL_UINT16(6, pop_str());
    TEST_ASSERT_EQUAL_STRING("hello\n", s_out);
    TEST_ASSERT_EQUAL_UINT32(0, ls_ring_count(&s_r));
}

static void test_pop_empty_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, pop_str());
}

static void test_fifo_order(void)
{
    push_str("a1\n");
    push_str("b2\n");
    push_str("c3\n");
    pop_str();
    TEST_ASSERT_EQUAL_STRING("a1\n", s_out);
    pop_str();
    TEST_ASSERT_EQUAL_STRING("b2\n", s_out);
    pop_str();
    TEST_ASSERT_EQUAL_STRING("c3\n", s_out);
}

static void test_full_ring_evicts_oldest_counted(void)
{
    /* 64 B ring: each 10 B record costs 12 B -> 5 fit */
    uint32_t evicted = 0;

    for (int i = 0; i < 8; i++)
    {
        char line[16];

        snprintf(line, sizeof(line), "line-%d***\n", i);
        evicted += ls_ring_push(&s_r, line, 10);
    }

    TEST_ASSERT_EQUAL_UINT32(3, evicted);
    TEST_ASSERT_EQUAL_UINT32(5, ls_ring_count(&s_r));
    pop_str();
    TEST_ASSERT_EQUAL_STRING_LEN("line-3", s_out, 6); /* 0..2 evicted */
}

static void test_wrap_keeps_records_intact(void)
{
    /* uneven sizes force the head to wrap mid-record repeatedly */
    for (int lap = 0; lap < 20; lap++)
    {
        char line[32];
        int len = 5 + (lap * 7) % 23;

        memset(line, 'a' + lap % 26, sizeof(line));
        ls_ring_push(&s_r, line, (uint16_t)len);

        if (lap % 3 == 0)
        {
            uint16_t n = pop_str();

            TEST_ASSERT_GREATER_THAN(0, n);

            for (uint16_t i = 0; i < n; i++)
            {
                TEST_ASSERT_EQUAL(s_out[0], s_out[i]);
            }
        }
    }
}

static void test_oversized_record_truncated_to_fit(void)
{
    char big[100];

    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL_UINT32(0, ls_ring_push(&s_r, big, sizeof(big)));
    TEST_ASSERT_EQUAL_UINT16(62, pop_str()); /* 64 - 2 B header */
}

static void test_pop_small_buffer_truncates_discards_tail(void)
{
    push_str("0123456789\n");

    char tiny[4];

    TEST_ASSERT_EQUAL_UINT16(4, ls_ring_pop(&s_r, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_UINT32(0, ls_ring_count(&s_r));
    TEST_ASSERT_EQUAL_UINT8('0', tiny[0]);
    TEST_ASSERT_EQUAL_UINT8('3', tiny[3]);
}

static void test_batch_joins_whole_records_within_budget(void)
{
    push_str("aaaa\n");
    push_str("bbbb\n");
    push_str("cccc\n");

    uint32_t lines = 0;
    uint32_t n = ls_ring_pop_batch(&s_r, s_out, 12, &lines);

    TEST_ASSERT_EQUAL_UINT32(10, n); /* 2 whole records; the 3rd waits */
    TEST_ASSERT_EQUAL_UINT32(2, lines);
    TEST_ASSERT_EQUAL_UINT32(1, ls_ring_count(&s_r));
    TEST_ASSERT_EQUAL_MEMORY("aaaa\nbbbb\n", s_out, 10);

    n = ls_ring_pop_batch(&s_r, s_out, 12, &lines);
    TEST_ASSERT_EQUAL_UINT32(5, n);
    TEST_ASSERT_EQUAL_UINT32(1, lines);
}

static void test_batch_never_wedges_on_over_budget_record(void)
{
    push_str("0123456789abcdef\n"); /* 17 B > 8 B budget */
    push_str("z\n");

    uint32_t lines = 0;
    uint32_t n = ls_ring_pop_batch(&s_r, s_out, 8, &lines);

    TEST_ASSERT_EQUAL_UINT32(8, n); /* truncated, not stuck */
    TEST_ASSERT_EQUAL_UINT32(1, lines);
    n = ls_ring_pop_batch(&s_r, s_out, 8, &lines);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32(0, ls_ring_count(&s_r));
}

static void test_batch_empty_reports_zero_lines(void)
{
    uint32_t lines = 99;

    TEST_ASSERT_EQUAL_UINT32(0,
                             ls_ring_pop_batch(&s_r, s_out, 64, &lines));
    TEST_ASSERT_EQUAL_UINT32(0, lines);
}

static void test_next_epoch_monotonic_on_clock_rollback(void)
{
    TEST_ASSERT_EQUAL_UINT32(1000, ls_rotate_next_epoch(1000, 500));
    TEST_ASSERT_EQUAL_UINT32(501, ls_rotate_next_epoch(200, 500));
    TEST_ASSERT_EQUAL_UINT32(501, ls_rotate_next_epoch(500, 500));
    TEST_ASSERT_EQUAL_UINT32(1, ls_rotate_next_epoch(0, 0));
}

static void test_retention_victims_are_the_oldest(void)
{
    uint32_t epochs[] = { 50, 10, 40, 30, 20 };
    int victims = ls_rotate_victims(epochs, 5, 3);

    TEST_ASSERT_EQUAL_INT(2, victims);
    TEST_ASSERT_EQUAL_UINT32(10, epochs[0]);
    TEST_ASSERT_EQUAL_UINT32(20, epochs[1]);
    TEST_ASSERT_EQUAL_UINT32(30, epochs[2]); /* survivor boundary */
}

static void test_retention_under_cap_keeps_all(void)
{
    uint32_t epochs[] = { 3, 1, 2 };

    TEST_ASSERT_EQUAL_INT(0, ls_rotate_victims(epochs, 3, 4));
    TEST_ASSERT_EQUAL_INT(0, ls_rotate_victims(epochs, 3, 3));
}

static void test_retention_keep_clamps_to_one(void)
{
    uint32_t epochs[] = { 3, 1, 2 };

    TEST_ASSERT_EQUAL_INT(2, ls_rotate_victims(epochs, 3, 0));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_push_pop_roundtrip);
    RUN_TEST(test_pop_empty_returns_zero);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_full_ring_evicts_oldest_counted);
    RUN_TEST(test_wrap_keeps_records_intact);
    RUN_TEST(test_oversized_record_truncated_to_fit);
    RUN_TEST(test_pop_small_buffer_truncates_discards_tail);
    RUN_TEST(test_batch_joins_whole_records_within_budget);
    RUN_TEST(test_batch_never_wedges_on_over_budget_record);
    RUN_TEST(test_batch_empty_reports_zero_lines);
    RUN_TEST(test_next_epoch_monotonic_on_clock_rollback);
    RUN_TEST(test_retention_victims_are_the_oldest);
    RUN_TEST(test_retention_under_cap_keeps_all);
    RUN_TEST(test_retention_keep_clamps_to_one);
    UNITY_END();
}
