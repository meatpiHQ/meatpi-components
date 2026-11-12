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
 * @file test_settle.c
 * @brief Issue #1023, the device killer: no reset inside a boot of the chip.
 *        The READY settle logic against the traces the bench measured on two
 *        units (2026-11-12), and the verdict on a `VTVERS` answer that tells
 *        a chip with a damaged store from one to recover.
 *
 *        The first test group is the regression for the fault itself: the
 *        old bring-up read READY 300 ms behind the sleep pin and pulsed the
 *        reset line there; at that moment the logic here must still say
 *        MOVING, on every trace.
 */
#include <string.h>

#include "unity.h"

#include "obd_chip_private.h"

/** READY as the bench saw it: the level it starts on and the times (ms) at
 *  which it changes; `H0 L113 H231 L497` = { false, {113, 231, 497} }. */
typedef struct
{
    bool     ready_at_0;
    uint32_t change_ms[4];
    size_t   n;
} trace_t;

static bool level_at(const trace_t *t, uint32_t ms)
{
    bool ready = t->ready_at_0;

    for (size_t i = 0; i < t->n; i++)
    {
        if (ms >= t->change_ms[i])
        {
            ready = !ready;
        }
    }

    return ready;
}

/** Feed the trace every @p step_ms as the pins code does; the verdict and
 *  the time (ms) it came at, or MOVING at @p limit_ms. */
static obd_settle_verdict_t run(const trace_t *t, uint32_t step_ms,
                                uint32_t limit_ms, uint32_t *at_ms)
{
    obd_settle_t st;

    obd_settle_reset(&st);

    for (uint32_t ms = 0; ms <= limit_ms; ms += step_ms)
    {
        obd_settle_verdict_t v =
            obd_settle_feed(&st, level_at(t, ms), (int64_t)ms * 1000);

        if (v != OBD_SETTLE_MOVING)
        {
            *at_ms = ms;
            return v;
        }
    }

    *at_ms = limit_ms;
    return OBD_SETTLE_MOVING;
}

/* the wakes behind the sleep pin (unit 1, unit 2, unit 2 with its store to
   tidy: the boot in which a reset destroys the chip) */
static const trace_t WAKE_UNIT1 = { false, { 114, 237, 502 }, 3 };
static const trace_t WAKE_UNIT2 = { false, { 113, 231, 497 }, 3 };
static const trace_t WAKE_TIDY  = { false, { 113, 231, 678 }, 3 };
/* a chip that was awake all along (a power-on, a settings save) */
static const trace_t AWAKE      = { true, { 0 }, 0 };
/* a reset pulse on an awake chip: floating low, high from 19 ms, ready */
static const trace_t RESET_BOOT = { true, { 19, 285 }, 2 };
static const trace_t RESET_TIDY = { true, { 19, 459 }, 2 };
/* asleep or dead: READY never comes */
static const trace_t STUCK_HIGH = { false, { 0 }, 0 };

void test_settle_never_ready_where_the_old_bringup_pulsed(void)
{
    /* THE fault: 300 ms behind the sleep pin, and anywhere up to the end
       of the chip's own boot, the verdict must not be READY */
    const trace_t *wakes[] = { &WAKE_UNIT1, &WAKE_UNIT2, &WAKE_TIDY };

    for (size_t w = 0; w < 3; w++)
    {
        const uint32_t boot_end = wakes[w]->change_ms[2];
        obd_settle_t st;

        obd_settle_reset(&st);

        for (uint32_t ms = 0; ms < boot_end + OBD_READY_STEADY_MS; ms += 2)
        {
            obd_settle_verdict_t v =
                obd_settle_feed(&st, level_at(wakes[w], ms), (int64_t)ms * 1000);

            TEST_ASSERT_NOT_EQUAL_MESSAGE(OBD_SETTLE_READY, v,
                                          "READY before the chip's boot was over");
            TEST_ASSERT_NOT_EQUAL_MESSAGE(OBD_SETTLE_DOWN, v,
                                          "a waking chip taken for a stuck one");
        }
    }
}

void test_settle_a_wake_is_ready_behind_the_chips_boot(void)
{
    uint32_t at = 0;

    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, run(&WAKE_UNIT1, 2, OBD_WAKE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, 502 + OBD_READY_STEADY_MS, at);
    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, run(&WAKE_UNIT2, 2, OBD_WAKE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, 497 + OBD_READY_STEADY_MS, at);
    /* the wake with the store to tidy: ready only behind the longer boot */
    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, run(&WAKE_TIDY, 2, OBD_WAKE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, 678 + OBD_READY_STEADY_MS, at);
}

void test_settle_the_ready_of_a_waking_chip_is_too_short_to_pass(void)
{
    /* READY reads ready from 113 to 231..237 ms before the chip restarts
       itself: the steady time must be longer than that, with margin */
    TEST_ASSERT_GREATER_THAN_UINT32(2 * (237 - 113), OBD_READY_STEADY_MS);
    /* and "not ready" must outlast the longest boot seen (450 ms) twice */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2 * 450, OBD_DOWN_STEADY_MS);
    /* the limits leave room for the slowest wake (678 + steady) */
    TEST_ASSERT_GREATER_THAN_UINT32(2 * 678, OBD_WAKE_SETTLE_MS);
    TEST_ASSERT_GREATER_THAN_UINT32(459 + OBD_READY_STEADY_MS, OBD_PULSE_SETTLE_MS);
}

void test_settle_an_awake_chip_costs_the_old_300_ms(void)
{
    uint32_t at = 0;

    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, run(&AWAKE, 2, OBD_WAKE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, OBD_READY_STEADY_MS, at);
}

/* ---- the rule of the reset line: the last resort ------------------------------ */

void test_pulse_only_as_the_last_resort(void)
{
    /* a chip that was asked at both UART speeds and answered at neither,
       READY at rest: asleep or stuck for a full second, or reading ready */
    TEST_ASSERT_TRUE(obd_settle_pulse_allowed(OBD_SETTLE_DOWN, true));
    TEST_ASSERT_TRUE(obd_settle_pulse_allowed(OBD_SETTLE_READY, true));
    /* READY reading "asleep" is not enough: a chip that was not found
       silent gets no pulse (the bring-up pulsed here on READY alone until
       2026-11-12, as 4.x did) */
    TEST_ASSERT_FALSE(obd_settle_pulse_allowed(OBD_SETTLE_DOWN, false));
    /* nor a chip that reads ready and was not found silent (the nap's
       re-sleep, the crash park, the PowerEn repair and the warm start's
       fallback all pulsed here) */
    TEST_ASSERT_FALSE(obd_settle_pulse_allowed(OBD_SETTLE_READY, false));
    /* READY moves: a boot may be running, nobody gets a pulse */
    TEST_ASSERT_FALSE(obd_settle_pulse_allowed(OBD_SETTLE_MOVING, false));
    TEST_ASSERT_FALSE(obd_settle_pulse_allowed(OBD_SETTLE_MOVING, true));
}

void test_pulse_never_allowed_inside_a_measured_boot(void)
{
    /* every boot the bench measured, sampled as the pins code samples it,
       and asked at every sample, the chip silent or not (it is silent while
       it boots): no pulse may be allowed before the boot is over and READY
       has rested. The wake traces start with the sleep pin going high, the
       reset traces with a pulse. */
    static const struct
    {
        const trace_t *t;
        uint32_t       boot_end_ms;
    } BOOTS[] =
    {
        { &WAKE_UNIT1, 502 }, { &WAKE_UNIT2, 497 }, { &WAKE_TIDY, 678 },
        { &RESET_BOOT, 285 }, { &RESET_TIDY, 459 },
    };

    for (size_t b = 0; b < sizeof(BOOTS) / sizeof(BOOTS[0]); b++)
    {
        obd_settle_t st;

        obd_settle_reset(&st);

        /* a reset trace starts on the old boot's ready level: a caller that
           asks then has waited OBD_READY_STEADY_MS already, so the trace is
           fed from its first change on */
        const uint32_t from = BOOTS[b].t->ready_at_0 ? BOOTS[b].t->change_ms[0] : 0;

        for (uint32_t ms = from; ms < BOOTS[b].boot_end_ms + OBD_READY_STEADY_MS; ms += 2)
        {
            obd_settle_verdict_t v =
                obd_settle_feed(&st, level_at(BOOTS[b].t, ms), (int64_t)ms * 1000);

            TEST_ASSERT_FALSE_MESSAGE(obd_settle_pulse_allowed(v, true),
                                      "a pulse for a silent chip inside a boot");
            TEST_ASSERT_FALSE_MESSAGE(obd_settle_pulse_allowed(v, false),
                                      "a pulse for a chip that was not asked");
        }
    }
}

void test_pulse_a_silent_chip_asleep_waits_a_full_second(void)
{
    obd_settle_t st;

    obd_settle_reset(&st);

    for (uint32_t ms = 0; ms < OBD_DOWN_STEADY_MS; ms += 2)
    {
        TEST_ASSERT_FALSE(obd_settle_pulse_allowed(
            obd_settle_feed(&st, false, (int64_t)ms * 1000), true));
    }

    obd_settle_verdict_t v =
        obd_settle_feed(&st, false, (int64_t)OBD_DOWN_STEADY_MS * 1000);

    TEST_ASSERT_TRUE(obd_settle_pulse_allowed(v, true));
    /* and for all that second of "asleep", not without the look on the wire */
    TEST_ASSERT_FALSE(obd_settle_pulse_allowed(v, false));
}

void test_reset_line_has_one_driver_and_two_last_resort_calls(void)
{
    /* Counted in the component's sources when this suite is configured
       (host_test/main/CMakeLists.txt). A reset inside a boot of the chip
       destroys it, so who can pull the line is part of the tests: add a
       driver, a caller or an API and this fails. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, OBD_SRC_RESET_PIN_OUTSIDE_PINS,
        "the reset pin is named outside obd_chip_pins.c");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, OBD_SRC_RESET_PIN_LOW_WRITES,
        "obd_chip_pins.c pulls the reset line low in more than one place");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, OBD_SRC_PULSE_NAMED_ELSEWHERE,
        "the reset pulse is named outside the pins file, the bring-up and its header");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, OBD_SRC_PULSE_CALLS_IN_BRINGUP,
        "the bring-up has its last resort and the end of an update, no third call");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, OBD_SRC_RESET_LINE_INCLUDES,
        "obd_chip_reset_line.h is for obd_chip_pins.c and obd_chip_bringup.c alone");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, OBD_SRC_PUBLIC_RESET_PULSE_API,
        "a hardware reset of the chip is back in obd_chip.h");
}

void test_settle_a_reset_boot_is_waited_out(void)
{
    uint32_t at = 0;

    /* a second pulse asked for right behind a first one: it must wait for
       the boot the first one started, tidying included */
    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, run(&RESET_BOOT, 2, OBD_PULSE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, 285 + OBD_READY_STEADY_MS, at);
    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, run(&RESET_TIDY, 2, OBD_PULSE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, 459 + OBD_READY_STEADY_MS, at);
}

void test_settle_a_stuck_chip_is_down_and_a_restless_one_keeps_moving(void)
{
    uint32_t at = 0;

    TEST_ASSERT_EQUAL(OBD_SETTLE_DOWN, run(&STUCK_HIGH, 2, OBD_WAKE_SETTLE_MS, &at));
    TEST_ASSERT_UINT32_WITHIN(4, OBD_DOWN_STEADY_MS, at);

    /* a chip that resets itself every 100 ms (a bench suspect did): READY
       never holds, the caller gets MOVING at its limit */
    obd_settle_t st;

    obd_settle_reset(&st);

    for (uint32_t ms = 0; ms <= OBD_PULSE_SETTLE_MS; ms += 2)
    {
        bool ready = ((ms / 100) % 2) == 0;

        TEST_ASSERT_EQUAL(OBD_SETTLE_MOVING,
                          obd_settle_feed(&st, ready, (int64_t)ms * 1000));
    }
}

void test_settle_a_glitch_restarts_the_count(void)
{
    obd_settle_t st;

    obd_settle_reset(&st);
    TEST_ASSERT_EQUAL(OBD_SETTLE_MOVING, obd_settle_feed(&st, true, 0));
    TEST_ASSERT_EQUAL(OBD_SETTLE_MOVING, obd_settle_feed(&st, true, 299000));
    TEST_ASSERT_EQUAL(OBD_SETTLE_MOVING, obd_settle_feed(&st, false, 299500));
    TEST_ASSERT_EQUAL(OBD_SETTLE_MOVING, obd_settle_feed(&st, true, 300000));
    TEST_ASSERT_EQUAL(OBD_SETTLE_MOVING, obd_settle_feed(&st, true, 599000));
    TEST_ASSERT_EQUAL(OBD_SETTLE_READY, obd_settle_feed(&st, true, 600000));
}

/* ---- what a bring-up leaves behind as a fault code ---------------------------- */

void test_fault_a_silent_chip_is_latched_however_it_ended(void)
{
    char detail[48];

    /* a chip that answered: no fault, whatever else is said */
    TEST_ASSERT_FALSE(obd_fault_silent(false, false, true, detail, sizeof(detail)));
    TEST_ASSERT_FALSE(obd_fault_silent(false, true, true, detail, sizeof(detail)));

    /* the last resort brought it back: still a fault, the reset line was used */
    TEST_ASSERT_TRUE(obd_fault_silent(true, true, true, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("silent; came back after the reset pulse", detail);

    /* the dead chip of the bench */
    TEST_ASSERT_TRUE(obd_fault_silent(true, true, false, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("silent; still silent after the reset pulse", detail);

    /* READY never came to rest: the pulse was refused */
    TEST_ASSERT_TRUE(obd_fault_silent(true, false, false, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("silent; READY never at rest, no reset pulse", detail);

    /* a short buffer is cut, never overrun */
    char small[8];

    TEST_ASSERT_TRUE(obd_fault_silent(true, true, false, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("silent;", small);
    TEST_ASSERT_FALSE(obd_fault_silent(true, true, false, NULL, 0));
}

void test_fault_codes_and_details_fit_the_fault_store(void)
{
    /* dev_status_fault_t: code char[24], detail char[48] */
    TEST_ASSERT_LESS_THAN(24, sizeof(OBD_FAULT_SILENT));
    TEST_ASSERT_LESS_THAN(24, sizeof(OBD_FAULT_DAMAGED));
    TEST_ASSERT_LESS_OR_EQUAL(48, sizeof(OBD_FAULT_DAMAGED_DETAIL));
    TEST_ASSERT_EQUAL_STRING("obd_chip_silent", OBD_FAULT_SILENT);
    TEST_ASSERT_EQUAL_STRING("obd_chip_damaged", OBD_FAULT_DAMAGED);

    char detail[64];

    for (int pulsed = 0; pulsed < 2; pulsed++)
    {
        for (int found = 0; found < 2; found++)
        {
            TEST_ASSERT_TRUE(obd_fault_silent(true, pulsed != 0, found != 0, detail,
                                              sizeof(detail)));
            TEST_ASSERT_LESS_THAN(48, strlen(detail));
        }
    }
}

/* ---- the chip's answer to VTVERS --------------------------------------------- */

void test_vtvers_a_damaged_store_is_not_a_chip_to_recover(void)
{
    /* the five answers of the bench chip whose identity block was erased:
       a few bytes of whatever stands where its name was, then `ERR82`. A
       NUL among them must not hide the error text. */
    static const char A1[] = "d\xFA\x07\rERR82";
    static const char A2[] = "G\x80\x04\rERR82";
    static const char A3[] = { 'W', ' ', 0x04, (char)0xFC, 0x06, ',', 0x00,
                               (char)0xFF, 0x01, '\r', 'E', 'R', 'R', '8', '2' };
    static const char A4[] = { 'g', 0x00, 0x00, 0x00, '7', (char)0xCE, 0x00,
                               '\r', 'E', 'R', 'R', '8', '2' };
    static const char A5[] = "ERR82";

    TEST_ASSERT_EQUAL(OBD_VTVERS_DAMAGED, obd_fw_vtvers_class(A1, sizeof(A1) - 1));
    TEST_ASSERT_EQUAL(OBD_VTVERS_DAMAGED, obd_fw_vtvers_class(A2, sizeof(A2) - 1));
    TEST_ASSERT_EQUAL(OBD_VTVERS_DAMAGED, obd_fw_vtvers_class(A3, sizeof(A3)));
    TEST_ASSERT_EQUAL(OBD_VTVERS_DAMAGED, obd_fw_vtvers_class(A4, sizeof(A4)));
    TEST_ASSERT_EQUAL(OBD_VTVERS_DAMAGED, obd_fw_vtvers_class(A5, sizeof(A5) - 1));
}

void test_vtvers_versions_and_unknown_answers(void)
{
    static const char V22[] = "MIC3624 V2.3.22";
    static const char V24[] = "MIC3624 V2.3.24";
    static const char WHAT[] = "?";

    TEST_ASSERT_EQUAL(OBD_VTVERS_VERSION, obd_fw_vtvers_class(V22, sizeof(V22) - 1));
    TEST_ASSERT_EQUAL(OBD_VTVERS_VERSION, obd_fw_vtvers_class(V24, sizeof(V24) - 1));
    /* `?`: a chip in its download mode, or one without the word: the
       update's recovery path, as before */
    TEST_ASSERT_EQUAL(OBD_VTVERS_UNKNOWN, obd_fw_vtvers_class(WHAT, sizeof(WHAT) - 1));
    TEST_ASSERT_EQUAL(OBD_VTVERS_UNKNOWN, obd_fw_vtvers_class("", 0));
    TEST_ASSERT_EQUAL(OBD_VTVERS_UNKNOWN, obd_fw_vtvers_class(NULL, 0));
    /* two letters of it are not the error text, nor is it found past the end */
    TEST_ASSERT_EQUAL(OBD_VTVERS_UNKNOWN, obd_fw_vtvers_class("ER", 2));
    TEST_ASSERT_EQUAL(OBD_VTVERS_UNKNOWN, obd_fw_vtvers_class("xxERR", 4));
}

void test_question_alone_is_the_loaders_answer(void)
{
    /* a chip in its download mode answers every word with `?` (bench
       2026-11-12, a chip firmware update cut by a restart): with its line
       ends and its prompt, or as the extract hands it over */
    static const char BARE[] = "?";
    static const char RAW[] = "?\r\r>";
    static const char NUL_IN_IT[] = { '?', '\0', '\r' };

    TEST_ASSERT_TRUE(obd_fw_answer_is_question(BARE, sizeof(BARE) - 1));
    TEST_ASSERT_TRUE(obd_fw_answer_is_question(RAW, sizeof(RAW) - 1));
    TEST_ASSERT_TRUE(obd_fw_answer_is_question(NUL_IN_IT, sizeof(NUL_IN_IT)));

    /* a chip that runs its application answers the reset word with its
       banner, a word with OK, a damaged one with its error text */
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("ELM327 v2.3", 11));
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("OK", 2));
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("?ERR82", 6));
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("STOPPED", 7));

    /* no answer at all is not the loader's answer */
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("", 0));
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("?", 0));
    TEST_ASSERT_FALSE(obd_fw_answer_is_question("\r\r>", 3));
    TEST_ASSERT_FALSE(obd_fw_answer_is_question(NULL, 4));
}

void test_fw_image_records_are_counted_by_line(void)
{
    static const char IMG[] = "A1\nB2\nFFF1\n";

    TEST_ASSERT_EQUAL_UINT32(3, obd_fw_count_records(IMG, sizeof(IMG) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, obd_fw_count_records("", 0));
}
