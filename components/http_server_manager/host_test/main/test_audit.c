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
 * @file test_audit.c
 * @brief The flash audit's accounting: a request's counter deltas land on
 *        its route's row, an index past the table lands on the catch-all's
 *        row, a wrapped counter still counts, a row that never moved the
 *        counters is not "touched".
 */
#include <string.h>

#include "unity.h"

#include "http_server_manager_private.h"

static hsm_audit_row_t s_rows[4];

void test_audit_accounts_the_deltas_to_the_route(void)
{
    hsm_flash_ops_t before = { 10, 2, 1 };
    hsm_flash_ops_t after = { 13, 2, 1 };

    memset(s_rows, 0, sizeof(s_rows));
    hsm_audit_account(s_rows, 4, 1, &before, &after);
    hsm_audit_account(s_rows, 4, 1, &after, &after);
    TEST_ASSERT_EQUAL_UINT32(2, s_rows[1].requests);
    TEST_ASSERT_EQUAL_UINT32(3, s_rows[1].ops.reads);
    TEST_ASSERT_EQUAL_UINT32(0, s_rows[1].ops.writes);
    TEST_ASSERT_EQUAL_UINT32(0, s_rows[0].requests);
    TEST_ASSERT_TRUE(hsm_audit_row_touched_flash(&s_rows[1]));
    TEST_ASSERT_FALSE(hsm_audit_row_touched_flash(&s_rows[0]));
}

void test_audit_index_past_the_table_is_the_catch_all(void)
{
    hsm_flash_ops_t before = { 0, 0, 0 };
    hsm_flash_ops_t after = { 0, 1, 1 };

    memset(s_rows, 0, sizeof(s_rows));
    hsm_audit_account(s_rows, 4, 99, &before, &after);
    TEST_ASSERT_EQUAL_UINT32(1, s_rows[3].requests);
    TEST_ASSERT_EQUAL_UINT32(1, s_rows[3].ops.writes);
    TEST_ASSERT_EQUAL_UINT32(1, s_rows[3].ops.erases);
}

void test_audit_survives_a_counter_wrap(void)
{
    hsm_flash_ops_t before = { 0xFFFFFFFEu, 0, 0 };
    hsm_flash_ops_t after = { 1, 0, 0 };

    memset(s_rows, 0, sizeof(s_rows));
    hsm_audit_account(s_rows, 4, 0, &before, &after);
    TEST_ASSERT_EQUAL_UINT32(3, s_rows[0].ops.reads);
}

void test_audit_ignores_bad_arguments(void)
{
    hsm_flash_ops_t ops = { 1, 1, 1 };

    memset(s_rows, 0, sizeof(s_rows));
    hsm_audit_account(NULL, 4, 0, &ops, &ops);
    hsm_audit_account(s_rows, 0, 0, &ops, &ops);
    hsm_audit_account(s_rows, 4, 0, NULL, &ops);
    hsm_audit_account(s_rows, 4, 0, &ops, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, s_rows[0].requests);
    TEST_ASSERT_FALSE(hsm_audit_row_touched_flash(NULL));
}
