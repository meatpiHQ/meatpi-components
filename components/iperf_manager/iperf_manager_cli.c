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
 * @file iperf_manager_cli.c
 * @brief The `iperf` console command — hand-parsed argv over the managed
 *        engine (one CLI-tracked session at a time; the engine itself
 *        supports more).
 *
 * Report routing: the engine prints interval/summary lines on ITS report
 * task via the weak `iperf_report_output` — we override it to chain to
 * the default stdout writer (serial console) AND cache the latest
 * period/summary under a mutex, so remote CLI sessions (ws_cli/TCP/BLE
 * — where async prints can't reach) poll `iperf -r`.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iperf.h"
#include "lwip/inet.h"

#include "cmdline_manager.h"

#include "iperf_manager.h"

/* ---- latest-report cache (fed by the report-task override) ---------------- */

typedef struct
{
    bool                   valid;
    bool                   summary;
    iperf_id_t             id;
    iperf_traffic_report_t traffic;
} ipm_last_report_t;

static ipm_last_report_t s_last;
static SemaphoreHandle_t s_last_lock;
static StaticSemaphore_t s_last_lock_buf;

static volatile iperf_id_t s_session = -1; /* CLI-tracked instance */

/** Weak-symbol override (engine report task context): default print +
 *  cache for `iperf -r`. */
void iperf_report_output(const iperf_report_t *report)
{
    iperf_default_report_output(report);

    if (report == NULL || s_last_lock == NULL ||
        (report->report_type != IPERF_REPORT_PERIOD &&
         report->report_type != IPERF_REPORT_SUMMARY))
    {
        return;
    }

    xSemaphoreTake(s_last_lock, portMAX_DELAY);
    s_last.valid = true;
    s_last.summary = (report->report_type == IPERF_REPORT_SUMMARY);
    s_last.id = report->instance_id;
    s_last.traffic = report->traffic;
    xSemaphoreGive(s_last_lock);
}

/** Engine state callback: IPERF_CLOSED fires on EVERY termination path
 *  (finished, aborted, socket error) — the SUMMARY report does not (an
 *  accept-timeout server dies without one; learned on the bench). */
static void on_state(iperf_id_t id, iperf_state_data_t *data, void *priv)
{
    (void)priv;

    if (data != NULL && data->state == IPERF_CLOSED && id == s_session)
    {
        s_session = -1;
    }
}

/* ---- helpers --------------------------------------------------------------- */

static void print_traffic(const ipm_last_report_t *r)
{
    /* SUMMARY: whole-run average (the engine leaves the LAST period in
       the period fields); period: that interval's rate */
    uint32_t start = r->summary ? 0 : r->traffic.period_start_sec;
    double bytes = r->summary ? (double)r->traffic.total_transfer_bytes
                              : r->traffic.period_bytes;
    uint32_t secs = r->traffic.end_sec - start;
    double mbits = (secs > 0) ? bytes * 8.0 / 1e6 / (double)secs : 0;

    cmdline_printf("iperf[%d] %s %lu-%lu sec  %.0f KBytes  %.2f Mbits/sec"
                   "  (total %llu KBytes)\n",
                   (int)r->id, r->summary ? "SUMMARY" : "period",
                   (unsigned long)start,
                   (unsigned long)r->traffic.end_sec,
                   bytes / 1024.0, mbits,
                   (unsigned long long)(r->traffic.total_transfer_bytes /
                                        1024));
}

static int usage(void)
{
    cmdline_printf(
        "usage:\n"
        "  iperf -s [-u] [-p port] [-t secs] [-i secs]          server\n"
        "  iperf -c <ip> [-u] [-p port] [-t secs] [-i secs]\n"
        "        [-l len] [-b Mbits/s]                          client\n"
        "  iperf -r        latest period/summary numbers\n"
        "  iperf -a        abort\n"
        "PC side must run iperf 2.x (port %d default) - NOT iperf3.\n",
        IPERF_DEFAULT_PORT);
    return 1;
}

/* ---- the command ------------------------------------------------------------ */

static int cmd_iperf(int argc, char **argv)
{
    bool server = false;
    bool client = false;
    bool udp = false;
    const char *host = NULL;
    long port = IPERF_DEFAULT_PORT;
    long secs = IPERF_DEFAULT_TIME;
    long interval = IPERF_DEFAULT_INTERVAL;
    long len = 0;
    long bw_mbps = -1;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        bool more = (i + 1) < argc;

        if (strcmp(a, "-s") == 0)
        {
            server = true;
        }
        else if (strcmp(a, "-c") == 0 && more)
        {
            client = true;
            host = argv[++i];
        }
        else if (strcmp(a, "-u") == 0)
        {
            udp = true;
        }
        else if (strcmp(a, "-p") == 0 && more)
        {
            port = strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(a, "-t") == 0 && more)
        {
            secs = strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(a, "-i") == 0 && more)
        {
            interval = strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(a, "-l") == 0 && more)
        {
            len = strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(a, "-b") == 0 && more)
        {
            bw_mbps = strtol(argv[++i], NULL, 10);
        }
        else if (strcmp(a, "-a") == 0)
        {
            (void)iperf_stop_instance(IPERF_ALL_INSTANCES_ID);
            s_session = -1;
            cmdline_printf("aborted\n");
            return 0;
        }
        else if (strcmp(a, "-r") == 0)
        {
            ipm_last_report_t r;

            xSemaphoreTake(s_last_lock, portMAX_DELAY);
            r = s_last;
            xSemaphoreGive(s_last_lock);

            if (!r.valid)
            {
                cmdline_printf("no report yet (session %s)\n",
                               s_session >= 0 ? "running" : "none");
                return 0;
            }

            print_traffic(&r);
            return 0;
        }
        else
        {
            return usage();
        }
    }

    if (server == client) /* neither or both */
    {
        return usage();
    }

    if (port <= 0 || port > 65535 || secs <= 0 || interval <= 0 ||
        len < 0 || len > 65535)
    {
        cmdline_printf("bad argument value\n");
        return 1;
    }

    if (s_session >= 0)
    {
        cmdline_printf("session %d already running - `iperf -a` first\n",
                       (int)s_session);
        return 1;
    }

    iperf_cfg_t cfg = { 0 };

    cfg.state_handler = on_state;
    cfg.format = MBITS_PER_SEC;
    cfg.interval = (uint32_t)interval;
    cfg.time = (uint32_t)secs;
    cfg.len_send_buf = (uint16_t)len;
    cfg.bw_lim = (bw_mbps > 0) ? (int32_t)(bw_mbps * 1000000)
                               : IPERF_DEFAULT_NO_BW_LIMIT;
    cfg.flag = (udp ? IPERF_FLAG_UDP : IPERF_FLAG_TCP) |
               (server ? IPERF_FLAG_SERVER : IPERF_FLAG_CLIENT);

    if (client)
    {
        cfg.dport = (uint16_t)port;
        cfg.destination.type = ESP_IPADDR_TYPE_V4;
        cfg.destination.u_addr.ip4.addr = ipaddr_addr(host);

        if (cfg.destination.u_addr.ip4.addr == IPADDR_NONE)
        {
            cmdline_printf("bad host '%s' (IPv4 dotted quad only)\n", host);
            return 1;
        }
    }
    else
    {
        cfg.sport = (uint16_t)port; /* listens on every netif (0.0.0.0) */
    }

    xSemaphoreTake(s_last_lock, portMAX_DELAY);
    s_last.valid = false; /* fresh session, fresh numbers */
    xSemaphoreGive(s_last_lock);

    iperf_id_t id = iperf_start_instance(&cfg);

    if (id < 0)
    {
        cmdline_printf("failed to start (network up? port busy?)\n");
        return 1;
    }

    s_session = id;
    cmdline_printf("iperf[%d] %s %s started (%s%s port %ld, %ld s, "
                   "interval %ld s)\n",
                   (int)id, udp ? "udp" : "tcp",
                   server ? "server" : "client",
                   client ? host : "listen ",
                   client ? "" : "*", port, secs, interval);
    cmdline_printf("interval lines print on the serial console; "
                   "poll `iperf -r` here\n");
    return 0;
}

esp_err_t iperf_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "iperf",
        .help = "Link throughput (iperf2 peer): -s server / -c <ip> "
                "client / -r report / -a abort",
        .func = cmd_iperf,
    };

    if (s_last_lock == NULL)
    {
        s_last_lock = xSemaphoreCreateMutexStatic(&s_last_lock_buf);
    }

    return cmdline_manager_register(&CMD);
}
