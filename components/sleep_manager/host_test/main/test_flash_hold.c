/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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

/* An ECU flash holds the ladder past the activity cap; the critical floor
 * keeps the cap (sm_ladder_push, 2026-11-27). */
#include "unity.h"

#include "sleep_manager_private.h"

static void test_ecu_flash_holds_the_ladder_past_the_cap(void)
{
    sm_cfg_t cfg = { 0 };
    sm_policy_t p;
    sm_critical_t c;
    sm_activity_t a;

    cfg.sleep_v = 13.1f;
    cfg.wake_v = 13.2f;
    cfg.wake_hold_ms = 1000;
    cfg.delay_ms = 300000;
    cfg.interval_ms = 1800000;
    sm_policy_init(&p);
    sm_critical_init(&c);
    sm_activity_init(&a);
    sm_policy_eval(&p, &cfg, 11.55f, 0);   /* both count down: 5 min */
    sm_critical_eval(&c, 11.55f, 0);

    /* what hold_for_update does every second of a 30 min flash */
    for (uint32_t t = 1000; t < 1800000; t += 1000)
    {
        TEST_ASSERT_TRUE(sm_activity_apply(&p, true, &c, &a, t, 60000, 600000));
        sm_ladder_push(&p, true, t + 60000);
        TEST_ASSERT_EQUAL_INT(SM_ACT_NONE, sm_policy_eval(&p, &cfg, 11.55f, t));
    }

    /* the ladder followed the flash; the floor stopped at the cap */
    TEST_ASSERT_EQUAL_UINT32(1799000 + 60000, p.t_low);
    TEST_ASSERT_EQUAL_UINT32(1000 + 600000, c.t_trip);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, p.state);
}

static void test_ladder_push_only_while_it_counts(void)
{
    sm_policy_t p;

    sm_policy_init(&p);  /* NORMAL: nothing counts */
    p.t_low = 1234;
    sm_ladder_push(&p, true, 999999);
    TEST_ASSERT_EQUAL_UINT32(1234, p.t_low);

    p.state = SLEEP_MANAGER_LOW_VOLTAGE;
    sm_ladder_push(&p, false, 999999);   /* the ladder is off */
    TEST_ASSERT_EQUAL_UINT32(1234, p.t_low);

    sm_ladder_push(&p, true, 1000);      /* earlier: no change */
    TEST_ASSERT_EQUAL_UINT32(1234, p.t_low);
    sm_ladder_push(&p, true, 5000);
    TEST_ASSERT_EQUAL_UINT32(5000, p.t_low);
}

void sm_flash_hold_tests(void)
{
    RUN_TEST(test_ecu_flash_holds_the_ladder_past_the_cap);
    RUN_TEST(test_ladder_push_only_while_it_counts);
}
