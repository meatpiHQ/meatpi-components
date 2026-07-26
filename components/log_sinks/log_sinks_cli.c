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
 * @file log_sinks_cli.c
 * @brief The `logsinks` CLI command — registered on the settings apply
 *        (§6b). Bare = per-sink counter table (the conservation surface
 *        the bench asserts); `logsinks flush` = event-driven file flush;
 *        `logsinks emit <n> [gap_ms]` = numbered INFO lines (the bench's
 *        known generator).
 */
#include <stdlib.h>
#include <string.h>

#include "cmdline_manager.h"

#include "log_sinks.h"
#include "log_sinks_private.h"

static int cmd_logsinks(int argc, char **argv)
{
    static const char *NAMES[LOG_SINKS_COUNT] =
    {
        "tcp", "udp", "ws", "file"
    };
    static const char *DETAIL[LOG_SINKS_COUNT] =
    {
        "clients", "sendfail", "clients", "rotations"
    };

    if (argc >= 2 && strcmp(argv[1], "flush") == 0)
    {
        if (log_sinks_file_flush() != ESP_OK)
        {
            cmdline_printf("file sink not running\n");
            return 1;
        }

        cmdline_printf("flush requested\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "emit") == 0)
    {
        int n = atoi(argv[2]);
        int gap = (argc >= 4) ? atoi(argv[3]) : 5;

        if (n < 1 || n > 20000 || gap < 0 || gap > 1000)
        {
            cmdline_printf("usage: logsinks emit <1..20000> [gap_ms "
                           "0..1000]\n");
            return 1;
        }

        ls_emit_lines((uint32_t)n, (uint32_t)gap);
        cmdline_printf("emitted %d lines (gap %d ms)\n", n, gap);
        return 0;
    }

    cmdline_printf("sink  en in       out      dropped  buffered %s\n",
                   "detail");

    for (int i = 0; i < LOG_SINKS_COUNT; i++)
    {
        log_sinks_stats_t st;

        if (log_sinks_stats((log_sinks_id_t)i, &st) != ESP_OK)
        {
            continue;
        }

        cmdline_printf("%-5s %-2d %-8lu %-8lu %-8lu %-8lu %s=%lu\n",
                       NAMES[i], st.enabled, (unsigned long)st.in,
                       (unsigned long)st.out, (unsigned long)st.dropped,
                       (unsigned long)st.buffered, DETAIL[i],
                       (unsigned long)st.detail);
    }

    return 0;
}

esp_err_t log_sinks_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "logsinks",
        .help = "External log sink counters; `logsinks flush` / "
                "`logsinks emit <n> [gap_ms]`",
        .func = cmd_logsinks,
    };

    return cmdline_manager_register(&CMD);
}
