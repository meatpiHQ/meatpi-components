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
 * @file test_crash_core.c
 * @brief Host tests of the crash note's pure half: what counts as a note,
 *        how the next boot files it, lookup, and the note as text. The
 *        values of the reference note are those of the panic of 2026-10-05
 *        02:06:56 (TASK_j1939_wwh.md, finding 28).
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "restart_tracker_private.h"

static rt_crash_store_t s_store;

/** Stage A as the hook does it. */
static void stage_a(rt_crash_note_t *n, uint8_t kind)
{
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->core = 1;
    n->cause = 28; /* LoadProhibited */
    n->pc = 0x42209640U;
    n->excvaddr = 0x0000003CU;
    n->sp = 0x3C496F90U;
    n->ra = 0x82206860U;
    n->magic = RT_CRASH_NOTE_MAGIC;
    n->head_crc = rt_crash_head_crc(n);
}

/** A string into a zeroed fixed-width field, as the hook copies one. */
static void put(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);

    memcpy(dst, s, n < cap ? n : cap);
}

/** Stage B as the hook does it. */
static void stage_b(rt_crash_note_t *n, const char *reason, const char *text)
{
    static const uint32_t BT[7] =
    {
        0x4220963DU, 0x4220685DU, 0x42206805U, 0x420ED8F5U,
        0x420EC6A5U, 0x420EC945U, 0x4204E1C1U,
    };

    n->uptime_s = 18;
    memcpy(n->elf, "76a961dd5f08aabb", RT_CRASH_ELF_LEN); /* no terminator */
    put(n->task, sizeof(n->task), "apid_scan");
    put(n->reason, sizeof(n->reason), reason);
    put(n->text, sizeof(n->text), text);
    memcpy(n->bt, BT, sizeof(BT));
    n->bt_len = 7;
    n->tail_crc = rt_crash_tail_crc(n);
}

static void whole_note(rt_crash_note_t *n)
{
    stage_a(n, RESTART_TRACKER_CRASH_EXCEPTION);
    stage_b(n, "LoadProhibited", "");
}

void test_crash_layout(void)
{
    /* the numbers TASK_crash_note.md and the README state */
    TEST_ASSERT_EQUAL_UINT32(300, sizeof(rt_crash_note_t));
    TEST_ASSERT_EQUAL_UINT32(40, offsetof(rt_crash_note_t, uptime_s));
    TEST_ASSERT_EQUAL_UINT32(36, offsetof(rt_crash_note_t, head_crc));
    TEST_ASSERT_EQUAL_UINT32(296, offsetof(rt_crash_note_t, tail_crc));
    TEST_ASSERT_EQUAL_UINT32(2720, sizeof(rt_crash_store_t));
}

void test_crash_random_memory_is_no_note(void)
{
    memset(&s_store, 0xA5, sizeof(s_store)); /* power-on garbage */
    TEST_ASSERT_FALSE(rt_crash_store_valid(&s_store));
    TEST_ASSERT_FALSE(rt_crash_head_valid(&s_store.pending));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 0xA5A5A5A5U));

    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 1, 0, true));
    TEST_ASSERT_TRUE(rt_crash_store_valid(&s_store));

    /* all zero is no note either (a store that was just reset) */
    TEST_ASSERT_FALSE(rt_crash_head_valid(&s_store.pending));
    TEST_ASSERT_FALSE(rt_crash_tail_valid(&s_store.pending));
    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 2, 1, false));
}

void test_crash_head_crc_catches_a_flipped_bit(void)
{
    rt_crash_note_t n;

    whole_note(&n);
    TEST_ASSERT_TRUE(rt_crash_head_valid(&n));
    TEST_ASSERT_TRUE(rt_crash_tail_valid(&n));

    n.pc ^= 0x00000100U;
    TEST_ASSERT_FALSE(rt_crash_head_valid(&n));
    TEST_ASSERT_TRUE(rt_crash_tail_valid(&n)); /* the tail is its own */
}

void test_crash_tail_crc_catches_a_flipped_bit(void)
{
    rt_crash_note_t n;

    whole_note(&n);
    n.bt[3] ^= 0x00000001U;
    TEST_ASSERT_TRUE(rt_crash_head_valid(&n));
    TEST_ASSERT_FALSE(rt_crash_tail_valid(&n));

    whole_note(&n);
    n.bt_len = RT_CRASH_BT_LEN + 1; /* a length that cannot be, CRC redone */
    n.tail_crc = rt_crash_tail_crc(&n);
    TEST_ASSERT_FALSE(rt_crash_tail_valid(&n));
}

void test_crash_pending_lands_in_the_records_slot(void)
{
    rt_crash_store_reset(&s_store);
    whole_note(&s_store.pending);

    const rt_crash_note_t *filed = rt_crash_collect(&s_store, 7, 3, false);

    TEST_ASSERT_EQUAL_PTR(&s_store.kept[3], filed);
    TEST_ASSERT_EQUAL_UINT32(7, filed->sequence);
    TEST_ASSERT_TRUE(rt_crash_head_valid(filed));
    TEST_ASSERT_TRUE(rt_crash_tail_valid(filed));
    TEST_ASSERT_EQUAL_HEX32(0x42209640U, filed->pc);

    /* the pending slot is empty: the next boot files nothing */
    TEST_ASSERT_EACH_EQUAL_UINT8(0, &s_store.pending, sizeof(s_store.pending));
    TEST_ASSERT_EQUAL_PTR(filed, rt_crash_find(&s_store, 7));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 6));
    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 8, 4, false));
    TEST_ASSERT_EQUAL_PTR(filed, rt_crash_find(&s_store, 7)); /* still there */
}

void test_crash_head_only_note_is_filed_incomplete(void)
{
    rt_crash_store_reset(&s_store);
    stage_a(&s_store.pending, RESTART_TRACKER_CRASH_EXCEPTION);

    /* stage B died half way: fields written, no checksum */
    s_store.pending.uptime_s = 18;
    s_store.pending.bt[0] = 0x4220963DU;
    s_store.pending.bt_len = 1;
    strcpy(s_store.pending.task, "apid_scan");

    const rt_crash_note_t *filed = rt_crash_collect(&s_store, 9, 0, false);

    TEST_ASSERT_NOT_NULL(filed);
    TEST_ASSERT_TRUE(rt_crash_head_valid(filed));
    TEST_ASSERT_FALSE(rt_crash_tail_valid(filed));
    TEST_ASSERT_EQUAL_UINT8(0, filed->bt_len); /* what it left is dropped */
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)filed->task[0]);

    restart_tracker_crash_t c;

    rt_crash_export(filed, &c);
    TEST_ASSERT_FALSE(c.complete);
    TEST_ASSERT_EQUAL_UINT32(9, c.sequence);
    TEST_ASSERT_EQUAL_UINT8(RESTART_TRACKER_CRASH_EXCEPTION, c.kind);
    TEST_ASSERT_EQUAL_HEX32(0x42209640U, c.pc);
    TEST_ASSERT_EQUAL_HEX32(0x0000003CU, c.excvaddr);
    TEST_ASSERT_EQUAL_UINT32(28, c.cause);
    TEST_ASSERT_EQUAL_UINT8(1, c.core);
    TEST_ASSERT_EQUAL_UINT8(0, c.bt_len);
    TEST_ASSERT_EQUAL_STRING("", c.task);
}

void test_crash_no_pending_clears_the_slot(void)
{
    rt_crash_store_reset(&s_store);
    whole_note(&s_store.pending);
    TEST_ASSERT_NOT_NULL(rt_crash_collect(&s_store, 5, 2, false));

    /* eight boots later the ring is back at slot 2, with no crash */
    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 13, 2, false));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 5));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 13));
    TEST_ASSERT_EACH_EQUAL_UINT8(0, &s_store.kept[2], sizeof(s_store.kept[2]));
}

void test_crash_foreign_store_clears_kept_and_files_pending(void)
{
    rt_crash_store_reset(&s_store);
    whole_note(&s_store.pending);
    TEST_ASSERT_NOT_NULL(rt_crash_collect(&s_store, 3, 2, false));

    /* a layout change: the header is another version's */
    s_store.note_size = (uint16_t)(sizeof(rt_crash_note_t) - 4U);
    stage_a(&s_store.pending, RESTART_TRACKER_CRASH_ABORT);
    stage_b(&s_store.pending, "", "abort() was called at PC 0x4200d1c7");

    const rt_crash_note_t *filed = rt_crash_collect(&s_store, 4, 3, false);

    TEST_ASSERT_EQUAL_PTR(&s_store.kept[3], filed);
    TEST_ASSERT_TRUE(rt_crash_store_valid(&s_store));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 3)); /* the old note is gone */
    TEST_ASSERT_EQUAL_PTR(filed, rt_crash_find(&s_store, 4));
}

void test_crash_fresh_history_clears_kept_and_files_pending(void)
{
    rt_crash_store_reset(&s_store);
    whole_note(&s_store.pending);
    TEST_ASSERT_NOT_NULL(rt_crash_collect(&s_store, 1, 0, false));

    /* the tracker's state was invalid and starts again at boot 1: a note
       filed under the old boot 1 must not be found under the new one */
    whole_note(&s_store.pending);
    s_store.pending.pc = 0x42001234U;
    s_store.pending.head_crc = rt_crash_head_crc(&s_store.pending);

    const rt_crash_note_t *filed = rt_crash_collect(&s_store, 1, 0, true);

    TEST_ASSERT_NOT_NULL(filed);
    TEST_ASSERT_EQUAL_HEX32(0x42001234U, rt_crash_find(&s_store, 1)->pc);

    /* and with no pending note, the fresh history has none at all */
    whole_note(&s_store.pending);
    TEST_ASSERT_NOT_NULL(rt_crash_collect(&s_store, 2, 1, false));
    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 1, 0, true));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 1));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 2));
}

void test_crash_lookup_never_returns_another_boot(void)
{
    rt_crash_store_reset(&s_store);

    for (uint32_t seq = 1; seq <= RESTART_TRACKER_HISTORY_LEN; seq++)
    {
        whole_note(&s_store.pending);
        s_store.pending.uptime_s = seq; /* tells the notes apart */
        s_store.pending.tail_crc = rt_crash_tail_crc(&s_store.pending);
        TEST_ASSERT_NOT_NULL(rt_crash_collect(&s_store, seq, seq - 1U, false));
    }

    for (uint32_t seq = 1; seq <= RESTART_TRACKER_HISTORY_LEN; seq++)
    {
        TEST_ASSERT_EQUAL_UINT32(seq, rt_crash_find(&s_store, seq)->uptime_s);
    }

    /* the ring wraps: boot 9 takes slot 0, boot 1's note goes with it */
    whole_note(&s_store.pending);
    TEST_ASSERT_NOT_NULL(rt_crash_collect(&s_store, 9, 0, false));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 1));
    TEST_ASSERT_NOT_NULL(rt_crash_find(&s_store, 9));
    TEST_ASSERT_NOT_NULL(rt_crash_find(&s_store, 2));
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 0));  /* 0 = "pending / empty" */
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 10));

    /* a kept note whose head no longer checks is not returned */
    s_store.kept[1].pc ^= 1U;
    TEST_ASSERT_NULL(rt_crash_find(&s_store, 2));
}

void test_crash_bad_kind_or_slot_is_refused(void)
{
    rt_crash_note_t n;

    stage_a(&n, RESTART_TRACKER_CRASH_NONE);
    TEST_ASSERT_FALSE(rt_crash_head_valid(&n));
    stage_a(&n, RESTART_TRACKER_CRASH_DEBUG + 1);
    TEST_ASSERT_FALSE(rt_crash_head_valid(&n));

    /* a kept note is not a pending one: a sequence in the pending slot is
       a leftover, not a crash of the last run */
    rt_crash_store_reset(&s_store);
    whole_note(&s_store.pending);
    s_store.pending.sequence = 4;
    s_store.pending.head_crc = rt_crash_head_crc(&s_store.pending);
    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 5, 0, false));

    /* a slot outside the ring files nothing and still empties the pending */
    whole_note(&s_store.pending);
    TEST_ASSERT_NULL(rt_crash_collect(&s_store, 6,
                                      RESTART_TRACKER_HISTORY_LEN, false));
    TEST_ASSERT_FALSE(rt_crash_head_valid(&s_store.pending));
}

void test_crash_summary_exception(void)
{
    rt_crash_note_t n;
    restart_tracker_crash_t c;
    char line[240];

    whole_note(&n);
    n.sequence = 12;
    rt_crash_export(&n, &c);

    TEST_ASSERT_TRUE(c.complete);
    TEST_ASSERT_EQUAL_STRING("76a961dd5f08aabb", c.elf_sha);
    TEST_ASSERT_EQUAL_STRING("apid_scan", c.task);
    TEST_ASSERT_EQUAL_STRING("LoadProhibited", c.reason);
    TEST_ASSERT_EQUAL_UINT8(7, c.bt_len);
    TEST_ASSERT_EQUAL_HEX32(0x4220963DU, c.bt[0]);
    TEST_ASSERT_EQUAL_HEX32(0x4204E1C1U, c.bt[6]);
    TEST_ASSERT_FALSE(c.in_isr);
    TEST_ASSERT_FALSE(c.pseudo);

    int len = restart_tracker_crash_summary(&c, line, sizeof(line));

    TEST_ASSERT_EQUAL_STRING(
        "exception LoadProhibited at 0x42209640 (address 0x0000003c), "
        "core 1, task \"apid_scan\", up 18 s, image 76a961dd5f08aabb", line);
    TEST_ASSERT_EQUAL_INT((int)strlen(line), len);

    /* a short buffer truncates, it does not overrun */
    char tiny[16];

    memset(tiny, 0x55, sizeof(tiny));
    restart_tracker_crash_summary(&c, tiny, 12);
    TEST_ASSERT_EQUAL_STRING("exception L", tiny);
    TEST_ASSERT_EQUAL_HEX8(0x55, tiny[12]);

    TEST_ASSERT_EQUAL_INT(0, restart_tracker_crash_summary(NULL, line, 8));
    TEST_ASSERT_EQUAL_INT(0, restart_tracker_crash_summary(&c, line, 0));
}

void test_crash_summary_abort_watchdog_and_head_only(void)
{
    rt_crash_note_t n;
    restart_tracker_crash_t c;
    char line[240];

    stage_a(&n, RESTART_TRACKER_CRASH_ABORT);
    stage_b(&n, "", "assert failed: twai_node_get_info twai.c:412 (node)");
    n.tail_flags = RT_CRASH_TF_TEXT_CUT;
    n.tail_crc = rt_crash_tail_crc(&n);
    rt_crash_export(&n, &c);
    TEST_ASSERT_TRUE(c.text_cut);
    restart_tracker_crash_summary(&c, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING(
        "abort: assert failed: twai_node_get_info twai.c:412 (node)..., "
        "core 1, task \"apid_scan\", up 18 s, image 76a961dd5f08aabb", line);

    stage_a(&n, RESTART_TRACKER_CRASH_INT_WDT);
    n.head_flags = RT_CRASH_HF_PSEUDO;
    n.cause = 5; /* PANIC_RSN_INTWDT_CPU0 */
    n.core = 0;
    n.head_crc = rt_crash_head_crc(&n);
    stage_b(&n, "Interrupt wdt timeout on CPU0", "");
    n.tail_flags = RT_CRASH_TF_IN_ISR;
    n.bt2[0] = 0x40378F2AU;
    n.bt2_len = 1;
    n.bt2_core = 1;
    n.tail_crc = rt_crash_tail_crc(&n);
    rt_crash_export(&n, &c);
    TEST_ASSERT_TRUE(c.pseudo);
    TEST_ASSERT_TRUE(c.in_isr);
    TEST_ASSERT_EQUAL_UINT8(1, c.bt2_len);
    TEST_ASSERT_EQUAL_UINT8(1, c.bt2_core);
    TEST_ASSERT_EQUAL_STRING("Interrupt wdt timeout on CPU0", c.reason);
    restart_tracker_crash_summary(&c, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING(
        "interrupt watchdog at 0x42209640, core 0, task \"apid_scan\" "
        "(in an interrupt), up 18 s, image 76a961dd5f08aabb", line);

    /* the head alone still says what and where */
    stage_a(&n, RESTART_TRACKER_CRASH_EXCEPTION);
    rt_crash_export(&n, &c);
    restart_tracker_crash_summary(&c, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING(
        "exception, cause 28, at 0x42209640 (address 0x0000003c), core 1; "
        "the rest was not recorded", line);
}

void test_crash_text_is_made_printable(void)
{
    rt_crash_note_t n;
    restart_tracker_crash_t c;

    stage_a(&n, RESTART_TRACKER_CRASH_ABORT);
    stage_b(&n, "", "");
    memcpy(n.text, "line\none\x07\x80\xFFz", 12);

    /* fixed-width fields filled to the last byte carry no terminator */
    memset(n.task, 'T', sizeof(n.task));
    memset(n.reason, 'R', sizeof(n.reason));
    memset(n.elf, 'e', sizeof(n.elf));
    n.tail_crc = rt_crash_tail_crc(&n);
    rt_crash_export(&n, &c);

    TEST_ASSERT_EQUAL_STRING("line?one???z", c.text);
    TEST_ASSERT_EQUAL_UINT32(16, strlen(c.task));
    TEST_ASSERT_EQUAL_UINT32(32, strlen(c.reason));
    TEST_ASSERT_EQUAL_UINT32(16, strlen(c.elf_sha));

    memset(n.text, 'x', sizeof(n.text)); /* 96 bytes, no terminator */
    n.tail_crc = rt_crash_tail_crc(&n);
    rt_crash_export(&n, &c);
    TEST_ASSERT_EQUAL_UINT32(96, strlen(c.text));
}

void test_crash_kind_names(void)
{
    TEST_ASSERT_EQUAL_STRING("exception", restart_tracker_crash_kind_to_str(
                                 RESTART_TRACKER_CRASH_EXCEPTION));
    TEST_ASSERT_EQUAL_STRING("abort", restart_tracker_crash_kind_to_str(
                                 RESTART_TRACKER_CRASH_ABORT));
    TEST_ASSERT_EQUAL_STRING("int_wdt", restart_tracker_crash_kind_to_str(
                                 RESTART_TRACKER_CRASH_INT_WDT));
    TEST_ASSERT_EQUAL_STRING("task_wdt", restart_tracker_crash_kind_to_str(
                                 RESTART_TRACKER_CRASH_TASK_WDT));
    TEST_ASSERT_EQUAL_STRING("debug", restart_tracker_crash_kind_to_str(
                                 RESTART_TRACKER_CRASH_DEBUG));
    TEST_ASSERT_EQUAL_STRING("none", restart_tracker_crash_kind_to_str(
                                 RESTART_TRACKER_CRASH_NONE));
    TEST_ASSERT_EQUAL_STRING("none", restart_tracker_crash_kind_to_str(99));
}
