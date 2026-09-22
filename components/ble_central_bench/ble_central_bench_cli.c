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
 * @file ble_central_bench_cli.c
 * @brief `blebench` console command: status | connect | disconnect |
 *        run <notify|write|read|tunnel_up|tunnel_down> [seconds] [size]
 *        [notify|indicate] (the last = how FFF3 is subscribed, tunnel only).
 *        Registered by on_apply when the `cli` setting is on (§6b).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"

#include "cmdline_manager.h"

#include "ble_central_bench_private.h"

static void print_status(void)
{
    bcb_status_t st;

    ble_central_bench_get_status(&st);
    cmdline_printf("ble_central_bench: %s, state %s\n",
                   ble_central_bench_is_enabled() ? "enabled" : "disabled",
                   ble_central_bench_state_name(st.state));
    cmdline_printf("  peer %s %s mtu %u itvl %.2f ms phy tx/rx %u/%u dle %u/%u %s%s rssi %d\n",
                   st.peer.name, st.peer.addr, st.peer.mtu, st.peer.itvl_units * 1.25,
                   st.peer.phy_tx, st.peer.phy_rx, st.peer.dle_tx, st.peer.dle_rx,
                   st.peer.secured ? "secured" : "open", st.peer.bonded ? " bonded" : "",
                   st.peer.rssi);
    cmdline_printf("  chars fff1 %u fff2 %u fff3 %u fff4 %u dis %u\n",
                   st.chars.fff1, st.chars.fff2, st.chars.fff3, st.chars.fff4,
                   st.chars.dis_mfr);
    cmdline_printf("  last %s%s: %s %lu B in %lu ms = %lu kbps, %lu ops, %lu errors, %lu retries%s%s\n",
                   ble_central_bench_mode_name(st.last.mode),
                   st.last.running ? " (running)" : "",
                   st.last.ok ? "ok" : "FAIL",
                   (unsigned long)st.last.bytes, (unsigned long)st.last.ms,
                   (unsigned long)st.last.kbps, (unsigned long)st.last.count,
                   (unsigned long)st.last.errors, (unsigned long)st.last.retries,
                   st.last.detail[0] ? " - " : "", st.last.detail);
    cmdline_printf("  counters: connects %lu disconnects %lu (last reason %d) pair ok/fail %lu/%lu "
                   "notify %lu/%lu B write %lu/%lu B tunnel rx/tx %lu/%lu resync %lu\n",
                   (unsigned long)st.counters.connects, (unsigned long)st.counters.disconnects,
                   st.counters.last_disconnect_reason,
                   (unsigned long)st.counters.pair_ok, (unsigned long)st.counters.pair_fail,
                   (unsigned long)st.counters.notify_rx, (unsigned long)st.counters.notify_bytes,
                   (unsigned long)st.counters.write_tx, (unsigned long)st.counters.write_bytes,
                   (unsigned long)st.counters.tunnel_frames_rx,
                   (unsigned long)st.counters.tunnel_frames_tx,
                   (unsigned long)st.counters.tunnel_resync);
}

static int cmd_blebench(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0)
    {
        print_status();
        return 0;
    }

    esp_err_t err;

    if (strcmp(argv[1], "connect") == 0)
    {
        err = ble_central_bench_connect();
    }
    else if (strcmp(argv[1], "disconnect") == 0)
    {
        err = ble_central_bench_disconnect();
    }
    else if (strcmp(argv[1], "run") == 0 && argc >= 3)
    {
        bcb_mode_t mode = ble_central_bench_mode_parse(argv[2]);

        if (mode == BCB_MODE_NONE)
        {
            cmdline_printf("modes: notify write read tunnel_up tunnel_down\n");
            return 1;
        }

        err = ble_central_bench_run(mode,
                                    argc >= 4 ? (uint32_t)atoi(argv[3]) : 10,
                                    argc >= 5 ? (uint32_t)atoi(argv[4]) : 65536,
                                    (argc >= 6 && strcmp(argv[5], "indicate") == 0)
                                        ? BCB_OUT_INDICATE : BCB_OUT_NOTIFY);
    }
    else
    {
        cmdline_printf("usage: blebench [status|connect|disconnect|run <mode> [seconds] [size] [notify|indicate]]\n");
        return 1;
    }

    cmdline_printf("%s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

esp_err_t ble_central_bench_register_cli(void)
{
    const esp_console_cmd_t cmd =
    {
        .command  = "blebench",
        .help     = "BLE central bench: blebench [status|connect|disconnect|run <mode> [s] [size]]",
        .hint     = NULL,
        .func     = cmd_blebench,
        .argtable = NULL,
    };

    return cmdline_manager_register(&cmd);
}
