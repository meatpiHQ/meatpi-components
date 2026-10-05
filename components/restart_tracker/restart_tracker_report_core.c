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
 * @file restart_tracker_report_core.c
 * @brief The stored crash report's pure half: the record that goes to NVS,
 *        the rule that decides whether a boot writes it (the wear guard),
 *        and the report as text. No IDF dependencies: compiled as-is by the
 *        host unit tests. restart_tracker_report.c does the NVS calls.
 *
 * The wear guard, whole. A crash is stored when it is not the one already
 * stored: another place, another image, another call chain. The same crash
 * again is stored only for news about it: the brake parked the device on
 * it, the stored copy had no date and this one has, or a day went by. And
 * whatever the news, at most RT_REPORT_BUDGET reports are stored until a
 * run settles: a crash loop costs that many small writes, then none.
 */
#include "restart_tracker_private.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- the record ---------------------------------------------------------------- */

uint32_t rt_report_crc(const rt_report_t *report)
{
    return rt_crc32_bytes((const uint8_t *)report,
                          offsetof(rt_report_t, crc));
}

bool rt_report_valid(const rt_report_t *report)
{
    return report->magic == RT_REPORT_MAGIC &&
           report->version == RT_REPORT_VERSION &&
           report->size == sizeof(rt_report_t) &&
           rt_report_crc(report) == report->crc &&
           rt_crash_head_valid(&report->note);
}

void rt_report_build(rt_report_t *out, const rt_crash_note_t *note,
                     int64_t now_unix, const char *firmware, uint8_t streak,
                     bool parked)
{
    memset(out, 0, sizeof(*out));
    out->magic = RT_REPORT_MAGIC;
    out->version = RT_REPORT_VERSION;
    out->size = (uint16_t)sizeof(rt_report_t);
    out->stored_unix = (now_unix >= RT_MIN_VALID_UNIX_TIME) ? now_unix : 0;
    out->streak = streak;
    out->parked = parked ? 1U : 0U;

    if (firmware != NULL)
    {
        size_t len = strlen(firmware);

        /* fixed width, no terminator when full: as the note's own fields */
        memcpy(out->firmware, firmware,
               (len < sizeof(out->firmware)) ? len : sizeof(out->firmware));
    }

    out->note = *note;
    out->crc = rt_report_crc(out);
}

/* ---- the wear guard ---------------------------------------------------------- */

uint32_t rt_report_identity(const rt_crash_note_t *note)
{
    /* what makes two crashes the same one: where, in which image, on which
       task, through which callers. Not when, not the boot number, not what
       the other core happened to be doing. */
    uint8_t id[1 + 3 * sizeof(uint32_t) + RT_CRASH_ELF_LEN + RT_CRASH_TASK_LEN +
               RT_CRASH_REASON_LEN + RT_CRASH_TEXT_LEN + 1 +
               RT_CRASH_BT_LEN * sizeof(uint32_t)];
    size_t at = 0;
    uint8_t frames = (note->bt_len <= RT_CRASH_BT_LEN) ? note->bt_len : 0U;

    memset(id, 0, sizeof(id));
    id[at++] = note->kind;
    memcpy(&id[at], &note->cause, sizeof(note->cause));
    at += sizeof(note->cause);
    memcpy(&id[at], &note->pc, sizeof(note->pc));
    at += sizeof(note->pc);
    memcpy(&id[at], &note->excvaddr, sizeof(note->excvaddr));
    at += sizeof(note->excvaddr);
    memcpy(&id[at], note->elf, sizeof(note->elf));
    at += sizeof(note->elf);
    memcpy(&id[at], note->task, sizeof(note->task));
    at += sizeof(note->task);
    memcpy(&id[at], note->reason, sizeof(note->reason));
    at += sizeof(note->reason);
    memcpy(&id[at], note->text, sizeof(note->text));
    at += sizeof(note->text);
    id[at++] = frames;
    memcpy(&id[at], note->bt, frames * sizeof(uint32_t));

    return rt_crc32_bytes(id, sizeof(id));
}

rt_report_decision_t rt_report_decide(const rt_report_t *stored,
                                      const rt_report_t *fresh,
                                      uint8_t budget)
{
    if (stored != NULL && rt_report_valid(stored) &&
        rt_report_identity(&stored->note) == rt_report_identity(&fresh->note))
    {
        bool park_news = fresh->parked != 0U && stored->parked == 0U;
        bool date_news = stored->stored_unix == 0 && fresh->stored_unix != 0;
        bool day_later = stored->stored_unix != 0 &&
                         fresh->stored_unix >=
                             stored->stored_unix + RT_REPORT_REFRESH_S;

        if (!park_news && !date_news && !day_later)
        {
            return RT_REPORT_KEEP;
        }
    }

    return (budget > 0U) ? RT_REPORT_STORE : RT_REPORT_NO_BUDGET;
}

/* ---- the report for its readers ------------------------------------------------ */

void rt_report_export(const rt_report_t *report, restart_tracker_report_t *out)
{
    memset(out, 0, sizeof(*out));
    out->stored_unix = report->stored_unix;
    out->streak = report->streak;
    out->parked = report->parked != 0U;
    rt_put_text(out->firmware, sizeof(out->firmware), report->firmware,
                sizeof(report->firmware));
    rt_crash_export(&report->note, &out->crash);
}

typedef struct
{
    char  *buf;
    size_t cap;
    size_t len; /* what the whole text needs, as snprintf counts */
} rt_text_t;

static void put(rt_text_t *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void put(rt_text_t *t, const char *fmt, ...)
{
    size_t room = (t->len < t->cap) ? t->cap - t->len : 0U;
    va_list ap;

    va_start(ap, fmt);

    int n = vsnprintf((room > 0U) ? t->buf + t->len : NULL, room, fmt, ap);

    va_end(ap);

    if (n > 0)
    {
        t->len += (size_t)n;
    }
}

static void put_frames(rt_text_t *t, const uint32_t *pcs, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        put(t, "%s0x%08" PRIx32, (i > 0U) ? " " : "", pcs[i]);
    }
}

int rt_report_text(const restart_tracker_report_t *report, const char *device,
                   char *buf, size_t cap)
{
    if (report == NULL || buf == NULL || cap == 0U)
    {
        return 0;
    }

    rt_text_t t = { .buf = buf, .cap = cap, .len = 0 };
    const restart_tracker_crash_t *crash = &report->crash;
    char line[224];

    buf[0] = '\0';
    put(&t, "WiCAN crash report\n");

    if (device != NULL && device[0] != '\0')
    {
        put(&t, "Device:    %s\n", device);
    }

    put(&t, "Firmware:  %s\n",
        (report->firmware[0] != '\0') ? report->firmware
                                      : "not the one that stored this report");
    put(&t, "Image:     %s\n",
        (crash->elf_sha[0] != '\0') ? crash->elf_sha : "not recorded");

    if (report->stored_unix > 0)
    {
        time_t when = (time_t)report->stored_unix;
        struct tm utc;

        memset(&utc, 0, sizeof(utc));
        gmtime_r(&when, &utc);

        if (strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S UTC", &utc) == 0U)
        {
            snprintf(line, sizeof(line), "%" PRId64, report->stored_unix);
        }

        put(&t, "Stored:    %s\n", line);
    }
    else
    {
        put(&t, "Stored:    the clock was not set\n");
    }

    if (report->streak >= 2U || report->parked)
    {
        put(&t, "Loop:      %u crashes in a row%s\n", (unsigned)report->streak,
            report->parked ? "; the device parked itself" : "");
    }

    restart_tracker_crash_summary(crash, line, sizeof(line));
    put(&t, "Crash:     %s\n", line);

    if (crash->bt_len > 0U)
    {
        put(&t, "Backtrace: ");
        put_frames(&t, crash->bt, crash->bt_len);
        put(&t, "%s\n", crash->bt_corrupt ? " (corrupt)"
                                          : (crash->bt_more ? " ..." : ""));
    }

    if (crash->bt2_len > 0U)
    {
        put(&t, "Core %u:    ", (unsigned)crash->bt2_core);
        put_frames(&t, crash->bt2, crash->bt2_len);
        put(&t, "\n");
    }

    return (int)t.len;
}
