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

/* The connection trial's state machine (wifi_manager_trial.c), driven with
   the driver's events and a clock (2026-10-06). */
#include <string.h>

#include "unity.h"

#include "wifi_manager_private.h"

static wm_trial_t s_t;

static void begin_idle(void)
{
    wm_trial_init(&s_t);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_CONNECT,
                          wm_trial_begin(&s_t, "Home", "letmein-please", 0,
                                         false, 1000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_CONNECTING, s_t.state);
    TEST_ASSERT_TRUE(wm_trial_active(&s_t));
}

void test_trial_connects_and_reports_address(void)
{
    begin_idle();
    wm_trial_on_associated(&s_t, 6, 3000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ASSOCIATED, s_t.state);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_FINISH,
                          wm_trial_on_got_ip(&s_t, "192.168.1.23", -61,
                                             5200));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_DONE, s_t.state);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_CONNECTED, s_t.result);
    TEST_ASSERT_EQUAL_STRING("192.168.1.23", s_t.ip);
    TEST_ASSERT_EQUAL_INT(-61, s_t.rssi);
    TEST_ASSERT_EQUAL_UINT8(6, s_t.channel);
    TEST_ASSERT_EQUAL_UINT32(4200, wm_trial_took_ms(&s_t));
    TEST_ASSERT_FALSE(wm_trial_active(&s_t));
    TEST_ASSERT_EQUAL_STRING("connected", wm_trial_result_name(s_t.result));
    TEST_ASSERT_EQUAL_STRING("done", wm_trial_state_name(s_t.state));
}

void test_trial_wrong_password_by_reason(void)
{
    const uint8_t reasons[] = { 204, 15, 2, 202 };

    for (size_t i = 0; i < sizeof(reasons); i++)
    {
        begin_idle();
        TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_FINISH,
                              wm_trial_on_disconnect(&s_t, reasons[i],
                                                     7100));
        TEST_ASSERT_EQUAL_INT(WM_TRIAL_PASSWORD, s_t.result);
        TEST_ASSERT_EQUAL_UINT8(reasons[i], s_t.reason);
        TEST_ASSERT_EQUAL_UINT32(6100, wm_trial_took_ms(&s_t));
    }
}

void test_trial_not_found_and_refused(void)
{
    begin_idle();
    wm_trial_on_disconnect(&s_t, 201, 3800);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_NOT_FOUND, s_t.result);

    begin_idle();
    wm_trial_on_disconnect(&s_t, 203, 3800);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_REFUSED, s_t.result);

    /* a drop after the association (the driver raises CONNECTED once the
       handshake passed): the router letting go, here a beacon timeout */
    begin_idle();
    wm_trial_on_associated(&s_t, 11, 2000);
    wm_trial_on_disconnect(&s_t, 200, 4000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_REFUSED, s_t.result);
    TEST_ASSERT_EQUAL_STRING("refused", wm_trial_result_name(s_t.result));
}

void test_trial_no_address_budget(void)
{
    begin_idle();
    wm_trial_on_associated(&s_t, 6, 3000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE, wm_trial_tick(&s_t, 10900));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ASSOCIATED, s_t.state);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_FINISH, wm_trial_tick(&s_t, 11000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_NO_IP, s_t.result);
    TEST_ASSERT_EQUAL_UINT32(10000, wm_trial_took_ms(&s_t));
}

void test_trial_total_budget(void)
{
    begin_idle();
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE, wm_trial_tick(&s_t, 20999));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_FINISH, wm_trial_tick(&s_t, 21000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_TIMEOUT, s_t.result);

    /* associated late: the total budget still ends it before the address
       budget would */
    begin_idle();
    wm_trial_on_associated(&s_t, 6, 15000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_FINISH, wm_trial_tick(&s_t, 21000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_TIMEOUT, s_t.result);
}

void test_trial_busy_station_lets_go_first(void)
{
    wm_trial_init(&s_t);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_begin(&s_t, "Home", "letmein-please", 0,
                                         true, 1000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_LEAVING, s_t.state);
    TEST_ASSERT_TRUE(wm_trial_active(&s_t));
    TEST_ASSERT_EQUAL_STRING("running", wm_trial_state_name(s_t.state));

    /* its own leave (reason 8) is the cue to connect, and the budget
       counts from there */
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_CONNECT,
                          wm_trial_on_disconnect(&s_t, 8, 1400));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_CONNECTING, s_t.state);
    wm_trial_on_got_ip(&s_t, "10.0.0.9", -50, 4400);
    TEST_ASSERT_EQUAL_UINT32(3000, wm_trial_took_ms(&s_t));
}

void test_trial_leave_that_never_comes(void)
{
    wm_trial_init(&s_t);
    wm_trial_begin(&s_t, "Home", "letmein-please", 0, true, 1000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE, wm_trial_tick(&s_t, 3999));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_CONNECT, wm_trial_tick(&s_t, 4000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_CONNECTING, s_t.state);
}

void test_trial_refuses_bad_credentials_and_a_second_run(void)
{
    wm_trial_init(&s_t);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_begin(&s_t, "", "letmein-please", 0, false,
                                         0));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_IDLE, s_t.state);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_begin(&s_t, "Home", "short", 0, false, 0));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_IDLE, s_t.state);

    char long_ssid[WM_SSID_LEN + 2];

    memset(long_ssid, 'a', sizeof(long_ssid) - 1);
    long_ssid[sizeof(long_ssid) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_begin(&s_t, long_ssid, "letmein-please", 0,
                                         false, 0));

    /* an open network: no password is allowed */
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_CONNECT,
                          wm_trial_begin(&s_t, "Cafe", "", 0, false, 0));

    /* one at a time */
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_begin(&s_t, "Home", "letmein-please", 0,
                                         false, 10));
    TEST_ASSERT_EQUAL_STRING("Cafe", s_t.ssid);
}

void test_trial_events_when_idle_or_done_are_not_ours(void)
{
    wm_trial_init(&s_t);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_on_disconnect(&s_t, 204, 100));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_on_got_ip(&s_t, "10.0.0.1", -40, 100));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE, wm_trial_tick(&s_t, 99999));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_IDLE, s_t.state);

    begin_idle();
    wm_trial_on_disconnect(&s_t, 204, 5000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_DONE, s_t.state);
    /* the result stays as it is whatever comes after */
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE,
                          wm_trial_on_got_ip(&s_t, "10.0.0.1", -40, 6000));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_PASSWORD, s_t.result);
    TEST_ASSERT_EQUAL_STRING("", s_t.ip);
    wm_trial_on_associated(&s_t, 1, 6000);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_DONE, s_t.state);
}

void test_trial_clock_wrap(void)
{
    wm_trial_init(&s_t);
    wm_trial_begin(&s_t, "Home", "letmein-please", 0, false, 0xFFFFF000u);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_NONE, wm_trial_tick(&s_t, 0x00000F00u));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_FINISH,
                          wm_trial_tick(&s_t, 0xFFFFF000u + 20000u));
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_TIMEOUT, s_t.result);
    TEST_ASSERT_EQUAL_UINT32(20000, wm_trial_took_ms(&s_t));
}

void test_trial_channel_hint_kept_and_bounded(void)
{
    /* the scan row's channel rides the trial (2026-10-08); outside the
       2.4 GHz range it means all channels */
    wm_trial_init(&s_t);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_ACT_CONNECT,
                          wm_trial_begin(&s_t, "Home", "letmein-please", 11,
                                         false, 1000));
    TEST_ASSERT_EQUAL_UINT8(11, s_t.hint);
    wm_trial_init(&s_t);
    wm_trial_begin(&s_t, "Home", "letmein-please", 14, false, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, s_t.hint);
    wm_trial_init(&s_t);
    wm_trial_begin(&s_t, "Home", "letmein-please", 0, true, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, s_t.hint);
    TEST_ASSERT_EQUAL_INT(WM_TRIAL_LEAVING, s_t.state);
}
