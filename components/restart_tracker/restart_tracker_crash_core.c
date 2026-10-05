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
 * @file restart_tracker_crash_core.c
 * @brief The crash note's pure half: validity of a note and of the store,
 *        the filing rules of the next boot, lookup, and the note as text.
 *        No IDF dependencies: compiled as-is by the host unit tests. The
 *        panic hooks that write a note live in restart_tracker_crash.c.
 */
#include "restart_tracker_private.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- validity --------------------------------------------------------------- */

uint32_t rt_crash_head_crc(const rt_crash_note_t *note)
{
    return rt_crc32_bytes((const uint8_t *)note,
                          offsetof(rt_crash_note_t, head_crc));
}

uint32_t rt_crash_tail_crc(const rt_crash_note_t *note)
{
    size_t start = offsetof(rt_crash_note_t, uptime_s);

    return rt_crc32_bytes((const uint8_t *)note + start,
                          offsetof(rt_crash_note_t, tail_crc) - start);
}

bool rt_crash_head_valid(const rt_crash_note_t *note)
{
    return note->magic == RT_CRASH_NOTE_MAGIC &&
           note->kind != RESTART_TRACKER_CRASH_NONE &&
           note->kind <= RESTART_TRACKER_CRASH_DEBUG &&
           rt_crash_head_crc(note) == note->head_crc;
}

bool rt_crash_tail_valid(const rt_crash_note_t *note)
{
    return note->bt_len <= RT_CRASH_BT_LEN &&
           note->bt2_len <= RT_CRASH_BT2_LEN &&
           rt_crash_tail_crc(note) == note->tail_crc;
}

bool rt_crash_store_valid(const rt_crash_store_t *store)
{
    return store->magic == RT_CRASH_STORE_MAGIC &&
           store->version == RT_CRASH_VERSION &&
           store->note_size == sizeof(rt_crash_note_t);
}

void rt_crash_store_reset(rt_crash_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = RT_CRASH_STORE_MAGIC;
    store->version = RT_CRASH_VERSION;
    store->note_size = (uint16_t)sizeof(rt_crash_note_t);
    rt_brake_reset(&store->brake);
}

/* ---- the next boot: file the pending note ----------------------------------- */

const rt_crash_note_t *rt_crash_collect(rt_crash_store_t *store,
                                        uint32_t sequence, uint32_t slot,
                                        bool fresh_history)
{
    /* by value: the store may be reset below, and the note is checked on
       its own magic and CRC, whatever the store around it looks like */
    rt_crash_note_t note = store->pending;
    bool have = rt_crash_head_valid(&note) && note.sequence == 0U;

    if (!rt_crash_store_valid(store))
    {
        /* first power-on or a layout change: nothing here is ours */
        rt_crash_store_reset(store);
    }
    else if (fresh_history)
    {
        /* the history starts again at boot 1 (PSRAM lost, the store kept):
           old notes must not match new boot numbers. The brake's count
           stays: a firmware that loses its PSRAM at every boot is exactly
           what it has to stop. */
        memset(store->kept, 0, sizeof(store->kept));
    }

    if (!rt_brake_valid(&store->brake))
    {
        rt_brake_reset(&store->brake); /* scribbled on: count from zero */
    }

    /* a note only ever describes the run right before the boot that files
       it: the pending slot is empty from here to the next panic */
    memset(&store->pending, 0, sizeof(store->pending));

    if (slot >= RESTART_TRACKER_HISTORY_LEN)
    {
        return NULL;
    }

    rt_crash_note_t *dst = &store->kept[slot];

    memset(dst, 0, sizeof(*dst)); /* its record was just overwritten */

    if (!have)
    {
        return NULL;
    }

    if (!rt_crash_tail_valid(&note))
    {
        /* stage B never finished: keep the head, drop what it left */
        size_t tail = offsetof(rt_crash_note_t, uptime_s);

        memset((uint8_t *)&note + tail, 0, sizeof(note) - tail);
    }

    note.sequence = sequence;
    note.head_crc = rt_crash_head_crc(&note);
    *dst = note;
    return dst;
}

const rt_crash_note_t *rt_crash_find(const rt_crash_store_t *store,
                                     uint32_t sequence)
{
    if (sequence == 0U || !rt_crash_store_valid(store))
    {
        return NULL;
    }

    for (int i = 0; i < RESTART_TRACKER_HISTORY_LEN; i++)
    {
        const rt_crash_note_t *note = &store->kept[i];

        if (note->sequence == sequence && rt_crash_head_valid(note))
        {
            return note;
        }
    }

    return NULL;
}

/* ---- the note for its readers ------------------------------------------------ */

/** Copy a fixed-width field that may lack its terminator; anything that is
 *  not printable ASCII becomes '?' (the text ends up in JSON and on a
 *  console, and part of it came off a crashed stack). */
void rt_put_text(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    size_t n = 0;

    while (n < src_len && n + 1U < dst_cap && src[n] != '\0')
    {
        unsigned char c = (unsigned char)src[n];

        dst[n] = (c >= 0x20U && c <= 0x7EU) ? (char)c : '?';
        n++;
    }

    dst[n] = '\0';
}

void rt_crash_export(const rt_crash_note_t *note,
                     restart_tracker_crash_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sequence = note->sequence;
    out->kind = note->kind;
    out->core = note->core;
    out->pseudo = (note->head_flags & RT_CRASH_HF_PSEUDO) != 0U;
    out->cause = note->cause;
    out->pc = note->pc;
    out->excvaddr = note->excvaddr;

    if (!rt_crash_tail_valid(note))
    {
        return; /* head only: `complete` stays false */
    }

    out->complete = true;
    out->in_isr = (note->tail_flags & RT_CRASH_TF_IN_ISR) != 0U;
    out->nested = (note->tail_flags & RT_CRASH_TF_NESTED) != 0U;
    out->early = (note->tail_flags & RT_CRASH_TF_EARLY) != 0U;
    out->bt_corrupt = (note->tail_flags & RT_CRASH_TF_BT_CORRUPT) != 0U;
    out->bt_more = (note->tail_flags & RT_CRASH_TF_BT_MORE) != 0U;
    out->text_cut = (note->tail_flags & RT_CRASH_TF_TEXT_CUT) != 0U;
    out->uptime_s = note->uptime_s;
    out->bt_len = note->bt_len;
    out->bt2_len = note->bt2_len;
    out->bt2_core = note->bt2_core;
    rt_put_text(out->elf_sha, sizeof(out->elf_sha), note->elf,
                sizeof(note->elf));
    rt_put_text(out->task, sizeof(out->task), note->task, sizeof(note->task));
    rt_put_text(out->reason, sizeof(out->reason), note->reason,
                sizeof(note->reason));
    rt_put_text(out->text, sizeof(out->text), note->text, sizeof(note->text));
    memcpy(out->bt, note->bt, sizeof(out->bt));
    memcpy(out->bt2, note->bt2, sizeof(out->bt2));
}

const char *restart_tracker_crash_kind_to_str(uint8_t kind)
{
    switch (kind)
    {
        case RESTART_TRACKER_CRASH_EXCEPTION: return "exception";
        case RESTART_TRACKER_CRASH_ABORT:     return "abort";
        case RESTART_TRACKER_CRASH_INT_WDT:   return "int_wdt";
        case RESTART_TRACKER_CRASH_TASK_WDT:  return "task_wdt";
        case RESTART_TRACKER_CRASH_DEBUG:     return "debug";
        default:                              return "none";
    }
}

/** The kind in words, for a sentence. */
static const char *kind_words(uint8_t kind)
{
    switch (kind)
    {
        case RESTART_TRACKER_CRASH_EXCEPTION: return "exception";
        case RESTART_TRACKER_CRASH_ABORT:     return "abort";
        case RESTART_TRACKER_CRASH_INT_WDT:   return "interrupt watchdog";
        case RESTART_TRACKER_CRASH_TASK_WDT:  return "task watchdog";
        case RESTART_TRACKER_CRASH_DEBUG:     return "debug exception";
        default:                              return "crash";
    }
}

int restart_tracker_crash_summary(const restart_tracker_crash_t *crash,
                                  char *buf, size_t cap)
{
    if (crash == NULL || buf == NULL || cap == 0U)
    {
        return 0;
    }

    if (!crash->complete)
    {
        /* stage A only: what IDF's info and the exception frame gave */
        return snprintf(buf, cap,
                        "%s, cause %" PRIu32 ", at 0x%08" PRIx32
                        " (address 0x%08" PRIx32 "), core %u; "
                        "the rest was not recorded",
                        kind_words(crash->kind), crash->cause, crash->pc,
                        crash->excvaddr, (unsigned)crash->core);
    }

    char what[160];

    if (crash->kind == RESTART_TRACKER_CRASH_ABORT)
    {
        snprintf(what, sizeof(what), "abort: %s%s",
                 crash->text[0] != '\0' ? crash->text : "(no text)",
                 crash->text_cut ? "..." : "");
    }
    else if (crash->kind == RESTART_TRACKER_CRASH_EXCEPTION)
    {
        snprintf(what, sizeof(what),
                 "exception %s at 0x%08" PRIx32 " (address 0x%08" PRIx32 ")",
                 crash->reason[0] != '\0' ? crash->reason : "?", crash->pc,
                 crash->excvaddr);
    }
    else
    {
        snprintf(what, sizeof(what), "%s at 0x%08" PRIx32,
                 kind_words(crash->kind), crash->pc);
    }

    return snprintf(buf, cap,
                    "%s, core %u, task \"%s\"%s, up %" PRIu32
                    " s, image %s",
                    what, (unsigned)crash->core, crash->task,
                    crash->in_isr ? " (in an interrupt)" : "",
                    crash->uptime_s,
                    crash->elf_sha[0] != '\0' ? crash->elf_sha : "?");
}
