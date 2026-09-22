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
 * @file ble_http_cli.c
 * @brief `blehttp` console command: tunnel status + counters.
 */
#include "cmdline_manager.h"

#include "ble_http.h"

static int cmd_blehttp(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ble_http_status_t st;

    if (ble_http_status(&st) != ESP_OK)
    {
        cmdline_printf("status unavailable\n");
        return 1;
    }

    cmdline_printf("HTTP API over BLE (channel 'http', FFF3/FFF4)\n");
    cmdline_printf("  enabled     : %s\n", st.enabled ? "yes" : "no");
    cmdline_printf("  registered  : %s\n", st.registered ? "yes" : "no");
    cmdline_printf("  link        : %s (out %s)\n",
                   st.link_secured ? "paired client" : "-", st.out_mode);
    cmdline_printf("  active      : %s %s %s\n", st.busy ? "yes" : "idle",
                   st.method, st.path);

    if (st.busy && st.body_expected > 0)
    {
        cmdline_printf("  upload      : %lu / %lu B\n",
                       (unsigned long)st.body_received,
                       (unsigned long)st.body_expected);
    }

    cmdline_printf("  requests    : %lu (responses %lu, last status %d)\n",
                   (unsigned long)st.requests, (unsigned long)st.responses,
                   st.last_status);
    cmdline_printf("  errors      : %lu (aborts %lu, timeouts %lu, resync %lu)\n",
                   (unsigned long)st.errors, (unsigned long)st.aborts,
                   (unsigned long)st.timeouts, (unsigned long)st.resync);
    cmdline_printf("  bytes in/out: %lu / %lu\n",
                   (unsigned long)st.bytes_in, (unsigned long)st.bytes_out);
    cmdline_printf("  v2 stream   : holes %lu, credits rx %lu, stalls %lu\n",
                   (unsigned long)st.holes, (unsigned long)st.credits_rx,
                   (unsigned long)st.credit_stalls);
    cmdline_printf("OK\n");
    return 0;
}

esp_err_t ble_http_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "blehttp",
        .help = "HTTP-over-BLE tunnel status",
        .func = cmd_blehttp,
    };

    return cmdline_manager_register(&CMD);
}
