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
 * @file test_report_core.c
 * @brief Host tests of the stored crash report's pure half: the NVS record,
 *        what makes two crashes the same one, the wear guard (what a boot
 *        may write, and how little a crash loop can), and the report as the
 *        text a user sends on.
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "restart_tracker_private.h"

#define T0 1767225600LL /* 2026-01-01 00:00:00 UTC */

static rt_report_t s_stored;
static rt_report_t s_fresh;

/** The reference crash of test_crash_core.c, as the next boot filed it. */
static void note(rt_crash_note_t *n, uint32_t pc)
{
    static const uint32_t BT[7] =
    {
        0x4220963DU, 0x4220685DU, 0x42206805U, 0x420ED8F5U,
        0x420EC6A5U, 0x420EC945U, 0x4204E1C1U,
    };

    memset(n, 0, sizeof(*n));
    n->magic = RT_CRASH_NOTE_MAGIC;
    n->sequence = 7;
    n->kind = RESTART_TRACKER_CRASH_EXCEPTION;
    n->core = 1;
    n->cause = 28; /* LoadProhibited */
    n->pc = pc;
    n->excvaddr = 0x0000003CU;
    n->head_crc = rt_crash_head_crc(n);

    n->uptime_s = 18;
    memcpy(n->elf, "76a961dd5f08aabb", RT_CRASH_ELF_LEN); /* no terminator */
    memcpy(n->task, "apid_scan", 9);
    memcpy(n->reason, "LoadProhibited", 14);
    memcpy(n->bt, BT, sizeof(BT));
    n->bt_len = 7;
    n->tail_crc = rt_crash_tail_crc(n);
}

static void report(rt_report_t *r, uint32_t pc, int64_t when, bool parked)
{
    rt_crash_note_t n;

    note(&n, pc);
    rt_report_build(r, &n, when, "v6.00p_alfa-01", parked ? 3 : 1, parked);
}

void test_report_layout_and_validity(void)
{
    TEST_ASSERT_EQUAL_UINT32(360, sizeof(rt_report_t));
    TEST_ASSERT_EQUAL_UINT32(56, offsetof(rt_report_t, note));
    TEST_ASSERT_EQUAL_UINT32(356, offsetof(rt_report_t, crc));

    memset(&s_stored, 0xA5, sizeof(s_stored)); /* an NVS blob that is not ours */
    TEST_ASSERT_FALSE(rt_report_valid(&s_stored));
    memset(&s_stored, 0, sizeof(s_stored));
    TEST_ASSERT_FALSE(rt_report_valid(&s_stored));

    report(&s_stored, 0x42209640U, T0, false);
    TEST_ASSERT_TRUE(rt_report_valid(&s_stored));

    s_stored.note.pc ^= 0x10U; /* one flipped bit */
    TEST_ASSERT_FALSE(rt_report_valid(&s_stored));
    report(&s_stored, 0x42209640U, T0, false);

    s_stored.size = (uint16_t)(sizeof(rt_report_t) - 4U); /* another layout */
    s_stored.crc = rt_report_crc(&s_stored);
    TEST_ASSERT_FALSE(rt_report_valid(&s_stored));
    report(&s_stored, 0x42209640U, T0, false);

    s_stored.version = RT_REPORT_VERSION + 1U;
    s_stored.crc = rt_report_crc(&s_stored);
    TEST_ASSERT_FALSE(rt_report_valid(&s_stored));
    report(&s_stored, 0x42209640U, T0, false);

    /* a record around something that is no note is no report */
    s_stored.note.kind = RESTART_TRACKER_CRASH_NONE;
    s_stored.crc = rt_report_crc(&s_stored);
    TEST_ASSERT_FALSE(rt_report_valid(&s_stored));
}

void test_report_build_fields(void)
{
    rt_crash_note_t n;

    note(&n, 0x42209640U);

    rt_report_build(&s_fresh, &n, T0 + 5, "v6.00p", 2, false);
    TEST_ASSERT_TRUE(rt_report_valid(&s_fresh));
    TEST_ASSERT_EQUAL(T0 + 5, s_fresh.stored_unix);
    TEST_ASSERT_EQUAL_UINT8(2, s_fresh.streak);
    TEST_ASSERT_EQUAL_UINT8(0, s_fresh.parked);
    TEST_ASSERT_EQUAL_STRING("v6.00p", s_fresh.firmware);
    TEST_ASSERT_EQUAL_MEMORY(&n, &s_fresh.note, sizeof(n));

    /* a clock that was never set is no date */
    rt_report_build(&s_fresh, &n, 12345, NULL, 3, true);
    TEST_ASSERT_TRUE(rt_report_valid(&s_fresh));
    TEST_ASSERT_EQUAL(0, s_fresh.stored_unix);
    TEST_ASSERT_EQUAL_UINT8(1, s_fresh.parked);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)s_fresh.firmware[0]);

    /* a version that fills the field, and one that is longer */
    rt_report_build(&s_fresh, &n, T0, "0123456789abcdef0123456789abcdefXYZ", 1,
                    false);
    TEST_ASSERT_EQUAL_MEMORY("0123456789abcdef0123456789abcdef",
                             s_fresh.firmware, RT_REPORT_FW_LEN);
    TEST_ASSERT_TRUE(rt_report_valid(&s_fresh)); /* the field ended there */

    restart_tracker_report_t out;

    rt_report_export(&s_fresh, &out);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", out.firmware);
    TEST_ASSERT_EQUAL_HEX32(0x42209640U, out.crash.pc);
    TEST_ASSERT_TRUE(out.crash.complete);
}

void test_report_identity(void)
{
    rt_crash_note_t a;
    rt_crash_note_t b;

    note(&a, 0x42209640U);
    note(&b, 0x42209640U);

    /* when, which boot, which core's leftovers: the same crash */
    b.sequence = 99;
    b.uptime_s = 4000;
    b.core = 0;
    b.sp = 0x3FC00000U;
    b.bt2_len = 2;
    b.bt2[0] = 0x40381234U;
    b.bt[9] = 0xDEADBEEFU; /* past bt_len: not a frame */
    TEST_ASSERT_EQUAL_HEX32(rt_report_identity(&a), rt_report_identity(&b));

    note(&b, 0x42209644U); /* another place */
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));

    note(&b, 0x42209640U);
    b.elf[0] = '8'; /* another image */
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));

    note(&b, 0x42209640U);
    b.task[0] = 'x'; /* another task */
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));

    note(&b, 0x42209640U);
    b.bt[3] = 0x420ED900U; /* the same place through another caller */
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));

    note(&b, 0x42209640U);
    b.bt_len = 6; /* a shorter chain */
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));

    note(&b, 0x42209640U);
    b.kind = RESTART_TRACKER_CRASH_ABORT;
    memcpy(b.text, "assert failed: x y.c:12", 23); /* another message */
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));

    /* a frame count that cannot be: no frames, and no read past the note */
    note(&b, 0x42209640U);
    b.bt_len = 200;
    TEST_ASSERT_NOT_EQUAL(rt_report_identity(&a), rt_report_identity(&b));
}

void test_report_decide_first_same_and_other(void)
{
    report(&s_fresh, 0x42209640U, T0, false);

    /* nothing stored, or a blob that is not a report */
    TEST_ASSERT_EQUAL(RT_REPORT_STORE, rt_report_decide(NULL, &s_fresh, 4));
    memset(&s_stored, 0xA5, sizeof(s_stored));
    TEST_ASSERT_EQUAL(RT_REPORT_STORE, rt_report_decide(&s_stored, &s_fresh, 1));
    TEST_ASSERT_EQUAL(RT_REPORT_NO_BUDGET,
                      rt_report_decide(NULL, &s_fresh, 0));

    /* the same crash again, a minute later: flash is left alone, and that
       needs no budget */
    s_stored = s_fresh;
    report(&s_fresh, 0x42209640U, T0 + 60, false);
    TEST_ASSERT_EQUAL(RT_REPORT_KEEP, rt_report_decide(&s_stored, &s_fresh, 4));
    TEST_ASSERT_EQUAL(RT_REPORT_KEEP, rt_report_decide(&s_stored, &s_fresh, 0));

    /* another crash replaces it, budget allowing */
    report(&s_fresh, 0x42001234U, T0 + 60, false);
    TEST_ASSERT_EQUAL(RT_REPORT_STORE, rt_report_decide(&s_stored, &s_fresh, 1));
    TEST_ASSERT_EQUAL(RT_REPORT_NO_BUDGET,
                      rt_report_decide(&s_stored, &s_fresh, 0));
}

void test_report_decide_news_about_the_same_crash(void)
{
    report(&s_stored, 0x42209640U, T0, false);

    /* the brake parked the device on it: stored once more, to say so */
    report(&s_fresh, 0x42209640U, T0 + 30, true);
    TEST_ASSERT_EQUAL(RT_REPORT_STORE, rt_report_decide(&s_stored, &s_fresh, 4));
    TEST_ASSERT_EQUAL(RT_REPORT_NO_BUDGET,
                      rt_report_decide(&s_stored, &s_fresh, 0));
    s_stored = s_fresh;
    report(&s_fresh, 0x42209640U, T0 + 600, true); /* a failed retry */
    TEST_ASSERT_EQUAL(RT_REPORT_KEEP, rt_report_decide(&s_stored, &s_fresh, 4));

    /* the same crash on a later day: its date is news, 23 hours are not */
    report(&s_fresh, 0x42209640U, T0 + 30 + RT_REPORT_REFRESH_S - 3600, false);
    TEST_ASSERT_EQUAL(RT_REPORT_KEEP, rt_report_decide(&s_stored, &s_fresh, 4));
    report(&s_fresh, 0x42209640U, T0 + 30 + RT_REPORT_REFRESH_S, false);
    TEST_ASSERT_EQUAL(RT_REPORT_STORE, rt_report_decide(&s_stored, &s_fresh, 4));

    /* a stored copy without a date gets one; a copy with a date is never
       replaced by one without */
    report(&s_stored, 0x42209640U, 0, false);
    report(&s_fresh, 0x42209640U, T0, false);
    TEST_ASSERT_EQUAL(RT_REPORT_STORE, rt_report_decide(&s_stored, &s_fresh, 4));
    report(&s_stored, 0x42209640U, T0, false);
    report(&s_fresh, 0x42209640U, 0, false);
    TEST_ASSERT_EQUAL(RT_REPORT_KEEP, rt_report_decide(&s_stored, &s_fresh, 4));

    /* both without a date: nothing new */
    report(&s_stored, 0x42209640U, 0, false);
    TEST_ASSERT_EQUAL(RT_REPORT_KEEP, rt_report_decide(&s_stored, &s_fresh, 4));
}

/** A boot's filing step as restart_tracker_report.c does it; counts writes. */
static int file_one(rt_brake_state_t *brake, bool *have, uint32_t pc,
                    int64_t when, bool parked)
{
    report(&s_fresh, pc, when, parked);

    if (rt_report_decide(*have ? &s_stored : NULL, &s_fresh, brake->budget) !=
        RT_REPORT_STORE)
    {
        return 0;
    }

    s_stored = s_fresh;
    *have = true;
    TEST_ASSERT_TRUE(rt_brake_spend(brake));
    return 1;
}

void test_report_a_crash_loop_costs_a_bounded_number_of_writes(void)
{
    rt_brake_state_t brake;
    bool have = false;
    int writes = 0;

    /* the usual loop: the same crash, the third one parks, then a failed
       retry every ten minutes for ten days */
    rt_brake_reset(&brake);
    writes += file_one(&brake, &have, 0x42209640U, T0, false);
    writes += file_one(&brake, &have, 0x42209640U, T0 + 15, false);
    writes += file_one(&brake, &have, 0x42209640U, T0 + 30, true);
    TEST_ASSERT_EQUAL_INT(2, writes); /* the crash, then "parked" */

    for (int i = 1; i <= 1440; i++)
    {
        writes += file_one(&brake, &have, 0x42209640U, T0 + 30 + 600LL * i,
                           true);
    }

    TEST_ASSERT_EQUAL_INT(RT_REPORT_BUDGET, writes); /* two dates, no more */

    /* the worst loop: a different crash at every boot, never settled */
    rt_brake_reset(&brake);
    have = false;
    writes = 0;

    for (uint32_t i = 0; i < 10000U; i++)
    {
        writes += file_one(&brake, &have, 0x42000000U + 4U * i, T0 + 15LL * i,
                           false);
    }

    TEST_ASSERT_EQUAL_INT(RT_REPORT_BUDGET, writes);
    TEST_ASSERT_EQUAL_HEX32(0x42000000U + 4U * (RT_REPORT_BUDGET - 1U),
                            s_stored.note.pc); /* the last one that fitted */

    /* a run that settles gives the budget back: the next crash is stored */
    rt_brake_settle(&brake);
    TEST_ASSERT_EQUAL_INT(1, file_one(&brake, &have, 0x42777770U, T0, false));
}

void test_report_text(void)
{
    static const char EXPECT[] =
        "WiCAN crash report\n"
        "Device:    68ee8f5a653d\n"
        "Firmware:  v6.00p_alfa-01\n"
        "Image:     76a961dd5f08aabb\n"
        "Stored:    2026-01-01 01:01:01 UTC\n"
        "Loop:      3 crashes in a row; the device parked itself\n"
        "Crash:     exception LoadProhibited at 0x42209640 (address "
        "0x0000003c), core 1, task \"apid_scan\", up 18 s, image "
        "76a961dd5f08aabb\n"
        "Backtrace: 0x4220963d 0x4220685d 0x42206805 0x420ed8f5 0x420ec6a5 "
        "0x420ec945 0x4204e1c1\n";
    restart_tracker_report_t out;
    char text[RESTART_TRACKER_REPORT_TEXT_MAX];

    report(&s_stored, 0x42209640U, T0 + 3661, true);
    rt_report_export(&s_stored, &out);
    TEST_ASSERT_EQUAL_INT(T0 + 3661, out.stored_unix);
    TEST_ASSERT_TRUE(out.parked);
    TEST_ASSERT_EQUAL_UINT8(3, out.streak);

    int need = rt_report_text(&out, "68ee8f5a653d", text, sizeof(text));

    TEST_ASSERT_EQUAL_STRING(EXPECT, text);
    TEST_ASSERT_EQUAL_INT((int)strlen(EXPECT), need);

    /* snprintf semantics: a small buffer gets the start, terminated, and
       the length the whole text needs */
    char small[40];

    memset(small, 'x', sizeof(small));
    TEST_ASSERT_EQUAL_INT(need, rt_report_text(&out, "68ee8f5a653d", small,
                                               sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32(sizeof(small) - 1U, strlen(small));
    TEST_ASSERT_EQUAL_MEMORY(EXPECT, small, sizeof(small) - 1U);

    TEST_ASSERT_EQUAL_INT(0, rt_report_text(&out, NULL, text, 0));
    TEST_ASSERT_EQUAL_INT(0, rt_report_text(NULL, NULL, text, sizeof(text)));
}

void test_report_text_of_what_was_not_recorded(void)
{
    restart_tracker_report_t out;
    char text[RESTART_TRACKER_REPORT_TEXT_MAX];
    rt_crash_note_t n;

    /* a crash whose handler hung: the head only, stored by another image,
       before the clock was set, with the other core's frames, one crash */
    note(&n, 0x42209640U);
    memset((uint8_t *)&n + offsetof(rt_crash_note_t, uptime_s), 0,
           sizeof(n) - offsetof(rt_crash_note_t, uptime_s));
    n.tail_crc = 0xFFFFFFFFU; /* never written */
    rt_report_build(&s_stored, &n, 0, "", 1, false);
    rt_report_export(&s_stored, &out);
    TEST_ASSERT_FALSE(out.crash.complete);
    rt_report_text(&out, NULL, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING(
        "WiCAN crash report\n"
        "Firmware:  not the one that stored this report\n"
        "Image:     not recorded\n"
        "Stored:    the clock was not set\n"
        "Crash:     exception, cause 28, at 0x42209640 (address 0x0000003c), "
        "core 1; the rest was not recorded\n",
        text);

    /* a watchdog, with what the other core was doing, and a stack that
       went on past the frames kept */
    note(&n, 0x4037A1B2U);
    n.kind = RESTART_TRACKER_CRASH_INT_WDT;
    n.head_crc = rt_crash_head_crc(&n);
    n.tail_flags = RT_CRASH_TF_BT_MORE;
    n.bt_len = 2;
    n.bt2_len = 2;
    n.bt2_core = 0;
    n.bt2[0] = 0x40381234U;
    n.bt2[1] = 0x4038ABCDU;
    n.tail_crc = rt_crash_tail_crc(&n);
    rt_report_build(&s_stored, &n, T0, "v6.00p", 2, false);
    rt_report_export(&s_stored, &out);
    rt_report_text(&out, "68ee8f5a653d", text, sizeof(text));
    TEST_ASSERT_NOT_NULL(strstr(text, "Loop:      2 crashes in a row\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "Crash:     interrupt watchdog at "
                                      "0x4037a1b2, core 1"));
    TEST_ASSERT_NOT_NULL(strstr(text, "Backtrace: 0x4220963d 0x4220685d ...\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "Core 0:    0x40381234 0x4038abcd\n"));

    /* the longest report there can be fits the documented buffer */
    note(&n, 0x42209640U);
    n.kind = RESTART_TRACKER_CRASH_ABORT;
    n.head_crc = rt_crash_head_crc(&n);
    memset(n.text, 'A', sizeof(n.text));
    memset(n.task, 'T', sizeof(n.task));
    n.tail_flags = RT_CRASH_TF_TEXT_CUT | RT_CRASH_TF_BT_CORRUPT;
    n.bt_len = RT_CRASH_BT_LEN;
    n.bt2_len = RT_CRASH_BT2_LEN;
    n.tail_crc = rt_crash_tail_crc(&n);
    rt_report_build(&s_stored, &n, T0, "0123456789abcdef0123456789abcdef", 255,
                    true);
    rt_report_export(&s_stored, &out);

    int need = rt_report_text(&out, "68ee8f5a653d", text, sizeof(text));

    TEST_ASSERT_LESS_THAN_INT(RESTART_TRACKER_REPORT_TEXT_MAX, need);
    TEST_ASSERT_EQUAL_INT((int)strlen(text), need);
}
