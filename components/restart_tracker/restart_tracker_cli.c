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
 * @file restart_tracker_cli.c
 * @brief The component's CLI command (`restart_tracker`): registered
 *        into cmdline_manager by restart_tracker_register_cli() (main
 *        wires it in CLI compositions only). Legacy option interface
 *        preserved (-l/--latest, -a/--history, -p/--pending,
 *        -n/--count, --panic); bare = counters + latest. v6 flags are
 *        caller-defined, so they print as hex (legacy had named flags).
 *        A record that follows a crash prints its crash note under it;
 *        `--panic[=abort|fault|wdt]` crashes on purpose in the three ways
 *        the tracker tells apart (the bench gate's instrument). `--report`
 *        prints the crash report stored in NVS and the crash-loop brake's
 *        count, `--clear-report` forgets it; `--settle` and `--park-retry`
 *        are the bench's two knobs on the brake.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "argtable3/argtable3.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "cmdline_manager.h"

#include "restart_tracker.h"

static struct
{
    struct arg_lit *latest;
    struct arg_lit *history;
    struct arg_lit *pending;
    struct arg_str *panic;
    struct arg_lit *report;
    struct arg_lit *clear_report;
    struct arg_lit *settle;
    struct arg_int *park_retry;
    struct arg_int *count;
    struct arg_end *end;
} s_args;

static void format_timestamp(int64_t ts, bool valid, char *buf,
                             size_t len)
{
    struct tm utc;
    time_t t = (time_t)ts;

    if (!valid || ts <= 0)
    {
        snprintf(buf, len, "unsynced");
        return;
    }

    gmtime_r(&t, &utc);

    if (strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
    {
        snprintf(buf, len, "%" PRId64, ts);
    }
}

/** The crash note of a boot, under its record line: what and where, then
 *  the PCs as addr2line takes them. */
static void print_crash(uint32_t sequence)
{
    /* PSRAM: commands run one at a time on the dispatcher task */
    static restart_tracker_crash_t crash EXT_RAM_BSS_ATTR;
    static char line[224] EXT_RAM_BSS_ATTR;

    if (restart_tracker_get_crash(sequence, &crash) != ESP_OK)
    {
        return;
    }

    restart_tracker_crash_summary(&crash, line, sizeof(line));
    cmdline_printf("    crash: %s\n", line);

    if (crash.bt_len > 0)
    {
        cmdline_printf("    backtrace:");

        for (int i = 0; i < crash.bt_len; i++)
        {
            cmdline_printf(" 0x%08" PRIx32, crash.bt[i]);
        }

        cmdline_printf("%s\n", crash.bt_corrupt ? " (corrupt)"
                                                : (crash.bt_more ? " ..." : ""));
    }

    if (crash.bt2_len > 0)
    {
        cmdline_printf("    core %u meanwhile:", (unsigned)crash.bt2_core);

        for (int i = 0; i < crash.bt2_len; i++)
        {
            cmdline_printf(" 0x%08" PRIx32, crash.bt2[i]);
        }

        cmdline_printf("\n");
    }
}

static void print_record(const restart_tracker_record_t *r)
{
    char boot_ts[32];
    char req_ts[32];

    format_timestamp(r->boot_timestamp, r->time_valid != 0, boot_ts,
                     sizeof(boot_ts));
    format_timestamp(r->request_timestamp, r->request_timestamp > 0,
                     req_ts, sizeof(req_ts));
    cmdline_printf("[%" PRIu32 "] reset=%s planned=%s source=%s "
                   "flags=0x%" PRIx32 " request=%s uptime_ms=%" PRIu64
                   " boot=%s\n",
                   r->sequence,
                   restart_tracker_reset_reason_to_str(
                       r->actual_reset_reason),
                   restart_tracker_planned_reason_to_str(
                       (restart_tracker_planned_reason_t)
                           r->planned_reason),
                   restart_tracker_source_to_str(
                       (restart_tracker_source_t)r->source),
                   r->flags, req_ts, r->request_uptime_ms, boot_ts);

    if (r->boot_mode != RESTART_TRACKER_BOOT_NORMAL)
    {
        cmdline_printf("    mode: %s\n",
                       restart_tracker_boot_mode_to_str(r->boot_mode));
    }

    print_crash(r->sequence);
}

/** The crash-loop brake in one line: this boot's verdict, the count now. */
static void print_brake(void)
{
    restart_tracker_brake_t brake;

    if (restart_tracker_get_brake(&brake) != ESP_OK)
    {
        return;
    }

    cmdline_printf("Brake: verdict=%s streak=%u/%u parks=%u settled=%s "
                   "report_budget=%u\n",
                   restart_tracker_boot_mode_to_str(brake.verdict),
                   (unsigned)brake.streak,
                   (unsigned)RESTART_TRACKER_BRAKE_STREAK,
                   (unsigned)brake.parks, brake.settled ? "yes" : "no",
                   (unsigned)brake.report_budget);
}

/** `--report`: the crash report NVS holds, as the text a user sends on. */
static int print_report(void)
{
    /* PSRAM: commands run one at a time on the dispatcher task */
    static restart_tracker_report_t report EXT_RAM_BSS_ATTR;
    static char text[RESTART_TRACKER_REPORT_TEXT_MAX] EXT_RAM_BSS_ATTR;

    if (restart_tracker_get_report(&report) == ESP_OK)
    {
        int len = restart_tracker_report_text(&report, text, sizeof(text));

        if (len >= (int)sizeof(text))
        {
            len = (int)sizeof(text) - 1;
        }

        cmdline_write(text, (len > 0) ? (size_t)len : 0U);
    }
    else
    {
        cmdline_printf("No crash report stored\n");
    }

    print_brake();

    /* the store the report sits in: how full the NVS partition is */
    nvs_stats_t stats;

    if (nvs_get_stats(NULL, &stats) == ESP_OK)
    {
        cmdline_printf("NVS: %u of %u entries used, %u free\n",
                       (unsigned)stats.used_entries,
                       (unsigned)stats.total_entries,
                       (unsigned)stats.available_entries);
    }

    cmdline_printf("OK\n");
    return 0;
}

static int print_history(int max_count)
{
    static restart_tracker_state_t st EXT_RAM_BSS_ATTR; /* ~600 B */

    if (restart_tracker_get_state(&st) != ESP_OK)
    {
        cmdline_printf("Error: restart tracker unavailable\n");
        return 1;
    }

    cmdline_printf("Boots: %" PRIu32 ", unexpected resets: %" PRIu32
                   "\n", st.boot_count, st.unexpected_reset_count);

    int printed = 0;

    for (uint32_t n = 0; n < RESTART_TRACKER_HISTORY_LEN &&
                         printed < max_count; n++)
    {
        uint32_t idx = (st.latest_history_index +
                        RESTART_TRACKER_HISTORY_LEN - n) %
                       RESTART_TRACKER_HISTORY_LEN;

        if (st.history[idx].sequence == 0)
        {
            continue;
        }

        print_record(&st.history[idx]);
        printed++;
    }

    cmdline_printf("OK\n");
    return 0;
}

static int print_pending(void)
{
    static restart_tracker_state_t st EXT_RAM_BSS_ATTR;

    if (restart_tracker_get_state(&st) != ESP_OK)
    {
        cmdline_printf("Error: restart tracker unavailable\n");
        return 1;
    }

    const restart_tracker_pending_t *p = &st.pending_restart;

    if (!p->valid)
    {
        cmdline_printf("No pending restart intent\n");
        cmdline_printf("OK\n");
        return 0;
    }

    char req_ts[32];

    format_timestamp(p->requested_timestamp, p->time_valid != 0, req_ts,
                     sizeof(req_ts));
    cmdline_printf("Pending: planned=%s source=%s flags=0x%" PRIx32
                   " request=%s uptime_ms=%" PRIu64 "\n",
                   restart_tracker_planned_reason_to_str(
                       (restart_tracker_planned_reason_t)
                           p->planned_reason),
                   restart_tracker_source_to_str(
                       (restart_tracker_source_t)p->source),
                   p->flags, req_ts, p->requested_uptime_ms);
    cmdline_printf("OK\n");
    return 0;
}

/* ---- `--panic`: the three ways to crash on purpose --------------------------
 * noinline: the crash note's PC and the bench's decode name these. */

#define RT_CLI_FAULT_ADDR 0x0000BAD0U /* no memory there: StoreProhibited */

static volatile uint32_t s_panic_sink;

static void __attribute__((noinline)) panic_fault(void)
{
    volatile uint32_t *p = (volatile uint32_t *)RT_CLI_FAULT_ADDR;

    *p = 0xDEADU;
    s_panic_sink++;
}

static void __attribute__((noinline)) panic_wdt(void)
{
    portDISABLE_INTERRUPTS(); /* and never back: the interrupt watchdog */

    for (;;)
    {
        s_panic_sink++;
    }
}

static int cmd_restart_tracker(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);

    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.panic->count > 0)
    {
        const char *kind = s_args.panic->sval[0]; /* "" = bare --panic */
        bool fault = strcmp(kind, "fault") == 0;
        bool wdt = strcmp(kind, "wdt") == 0;

        if (kind[0] != '\0' && strcmp(kind, "abort") != 0 && !fault && !wdt)
        {
            cmdline_printf("Error: --panic takes abort, fault or wdt\n");
            return 1;
        }

        /* deliberately die UNANNOUNCED: the next boot must record an
           unplanned reset and file its crash note, this is the tracker's
           self-test */
        cmdline_printf("Triggering test panic (%s)...\n",
                       fault ? "fault" : (wdt ? "wdt" : "abort"));
        vTaskDelay(pdMS_TO_TICKS(300));

        if (fault)
        {
            panic_fault();
        }

        if (wdt)
        {
            panic_wdt();
        }

        abort();
    }

    if (s_args.settle->count > 0)
    {
        /* a bench that crashes the device on purpose says so first: its
           crashes must not add up to a park */
        if (!restart_tracker_settle(true))
        {
            cmdline_printf("Error: restart tracker unavailable\n");
            return 1;
        }

        cmdline_printf("Run marked settled\nOK\n");
        return 0;
    }

    if (s_args.park_retry->count > 0)
    {
        int seconds = s_args.park_retry->ival[0];

        if (seconds < 0 || seconds > 3600 ||
            restart_tracker_set_test_retry((uint16_t)seconds) != ESP_OK)
        {
            cmdline_printf("Error: --park-retry takes 10..3600 seconds "
                           "(0 = off)\n");
            return 1;
        }

        cmdline_printf("The next park ends by itself after %d s\nOK\n",
                       seconds);
        return 0;
    }

    if (s_args.clear_report->count > 0)
    {
        esp_err_t err = restart_tracker_clear_report();

        if (err != ESP_OK)
        {
            cmdline_printf("Error: %s\n", esp_err_to_name(err));
            return 1;
        }

        cmdline_printf("Stored crash report cleared\nOK\n");
        return 0;
    }

    if (s_args.report->count > 0)
    {
        return print_report();
    }

    if (s_args.latest->count > 0)
    {
        restart_tracker_record_t rec;

        if (restart_tracker_get_latest_record(&rec) != ESP_OK)
        {
            cmdline_printf("Error: no boot record\n");
            return 1;
        }

        print_record(&rec);
        cmdline_printf("OK\n");
        return 0;
    }

    if (s_args.pending->count > 0)
    {
        return print_pending();
    }

    int count = RESTART_TRACKER_HISTORY_LEN;

    if (s_args.count->count > 0 && s_args.count->ival[0] > 0)
    {
        count = s_args.count->ival[0];
    }

    if (s_args.history->count > 0)
    {
        return print_history(count);
    }

    return print_history(1); /* bare: counters + the latest record */
}

esp_err_t restart_tracker_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "restart_tracker",
        .help = "Inspect retained restart tracker state",
        .hint = "Options: -l/--latest, -a/--history, -p/--pending, "
                "-n/--count <n>, --panic[=abort|fault|wdt], --report, "
                "--clear-report, --settle, --park-retry <s>",
        .func = cmd_restart_tracker,
        .argtable = &s_args,
    };

    s_args.latest = arg_lit0("l", "latest", "Show the latest boot record");
    s_args.history = arg_lit0("a", "history", "Show the boot history");
    s_args.pending = arg_lit0("p", "pending", "Show pending restart intent");
    s_args.panic = arg_str0(NULL, "panic", "<kind>",
                            "Crash on purpose (unplanned reset): abort "
                            "(default), fault, wdt");
    s_args.panic->hdr.flag |= ARG_HASOPTVALUE; /* --panic or --panic=<kind> */
    s_args.report = arg_lit0(NULL, "report",
                             "Show the stored crash report and the brake");
    s_args.clear_report = arg_lit0(NULL, "clear-report",
                                   "Forget the stored crash report");
    s_args.settle = arg_lit0(NULL, "settle",
                             "Mark this run healthy now (bench)");
    s_args.park_retry = arg_int0(NULL, "park-retry", "<s>",
                                 "The next crash park ends by itself after "
                                 "<s> seconds (bench)");
    s_args.count = arg_int0("n", "count", "<n>", "Max history records");
    s_args.end = arg_end(5);
    return cmdline_manager_register(&CMD);
}
