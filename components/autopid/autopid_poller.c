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
 * @file autopid_poller.c
 * @brief The poller task: the pauses (battery voltage, an external OBD
 *        client, a diagnostics hold, a chip job), the DTC due-check, then
 *        scheduler -> runner -> scheduler bookkeeping -> the failed-PID
 *        event. Split out of autopid.c 2026-10-02 (700-line rule);
 *        behaviour unchanged. Since 2026-10-03 the J1939 rows
 *        (autopid_j1939.h) run through every pause that is about the chip:
 *        they read the listener's store; a `?` row also asks the network
 *        (j1939 active mode) unless a diagnostic tool holds the bus.
 *
 * The task copies what it needs per iteration under the core's lock and
 * NEVER touches the filesystem (its stack is PSRAM; LittleFS access from a
 * PSRAM stack is the §2-corollary panic).
 */
#include "autopid.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "battery_monitor.h"
#include "dev_status_manager.h"

#include "obd_gate.h"
#include "autopid_private.h"
#include "autopid_poller.h"
#include "autopid_transport.h"

static const char *TAG = "autopid";

#define AP_IDLE_WAIT     pdMS_TO_TICKS(500)

/* the due entry's parameter slice, copied under the core's lock and
   consumed by the poller task alone: PSRAM, not a stack frame: at
   AP_PARAMS_PER 256 the slice is ~60 KB, larger than the whole task stack */
static ap_param_t  s_params_copy[AP_PARAMS_PER] EXT_RAM_BSS_ATTR;

static TaskHandle_t s_task;
static StaticTask_t s_tcb;             /* internal: FreeRTOS object */
static StackType_t  s_stack[12288] EXT_RAM_BSS_ATTR; /* PSRAM: 8192 left
                                only 508 B headroom (System Monitor,
                                2026-07-08) over ap_resp_to_payload's
                                ~5.6 KB line table: the dtc job stack
                                DID overflow on the same table
                                (2026-07-22), so buy real margin; no fs
                                I/O on this task so PSRAM is safe */

/* runtime */
static volatile bool s_paused_voltage;
static volatile bool s_paused_client;     /* an app is driving the chip */
static volatile bool s_scan_pause;        /* std scan owns the chip     */
static volatile bool s_paused_diag;       /* a diagnostic tool holds the bus (obd_gate) */
static volatile bool s_paused_bus;        /* the bus guard: nothing may be sent */
static QueueHandle_t s_batt_q;            /* voltage-pause events, or NULL */
static int64_t s_last_ok_us; /* last successful ECU poll (0 = never) */

/* ---- the poller task --------------------------------------------------------------- */

static void poller_task(void *arg)
{
    (void)arg;

    /* the tables, the scheduler and the counters stay in autopid.c */
    const ap_config_t *cfg = ap_core_config();
    ap_sched_t *sched = ap_core_sched();
    autopid_stats_t *stats = ap_core_stats();

    while (true)
    {
        /* voltage pause events (non-blocking drain) */
        if (s_batt_q != NULL)
        {
            battery_monitor_event_t ev;

            while (xQueueReceive(s_batt_q, &ev, 0) == pdTRUE)
            {
                bool below = (ev.type == BATTERY_MONITOR_EVENT_BELOW);

                if (below != s_paused_voltage)
                {
                    s_paused_voltage = below;
                    ESP_LOGI(TAG, "voltage pause %s (%.2f V)",
                             below ? "ON" : "OFF", ev.voltage);
                }
            }
        }

        /* yield to an external app (Car Scanner & co. over a bridge):
         * while it drives the chip our requests would interleave with
         * its conversations - it would read our lines as its answers and
         * our commands would STOP its requests (bench 2026-09-08). Legacy
         * parity: off the chip until the app has been silent
         * AP_CLIENT_YIELD_MS. */
        bool client = (ap_be()->client_idle_ms != NULL) &&
                      ap_sched_client_hold(ap_be()->client_idle_ms());

        if (client != s_paused_client)
        {
            s_paused_client = client;

            if (client)
            {
                ESP_LOGI(TAG, "paused: external OBD client active (resumes "
                         "%u s after its last command)",
                         (unsigned)(AP_CLIENT_YIELD_MS / 1000));
            }
            else
            {
                ESP_LOGI(TAG, "resumed: external OBD client idle");

                /* the app left the chip in ITS state (ATS0/ATH1/ATSH/
                   ATCRA...): restore the baseline the parser and the
                   PID inits assume before the first poll (bench
                   2026-09-08: every resumed poll failed on the app's
                   ATS0 otherwise). Only when we are going to poll. */
                if (ap_core_enabled() && !s_paused_voltage && !s_scan_pause)
                {
                    ap_runner_restore_baseline();
                }
            }
        }

        /* an ESP-side diagnostic tool (the UDS Tool, the J2534 PassThru
         * server) with its "exclusive" option on holds the bus for itself
         * (obd_gate's diagnostics hold): stay off the chip (polls AND DTC
         * scans) and acknowledge every loop so the tool can wait for us
         * to be off the bus before its first request. */
        bool diag = obd_gate_diag_held();

        if (diag != s_paused_diag)
        {
            s_paused_diag = diag;

            if (diag)
            {
                ESP_LOGI(TAG, "paused: a diagnostic tool holds the bus "
                              "(UDS Tool / J2534 exclusive)");
            }
            else
            {
                ESP_LOGI(TAG, "resumed: the diagnostic tool released the bus");

                /* the UDS chip transport re-addressed the chip (ATSH/
                   ATCRA/ATST): back to our baseline before polling */
                if (ap_core_enabled() && !s_paused_voltage && !s_scan_pause &&
                    !s_paused_client)
                {
                    ap_runner_restore_baseline();
                }
            }
        }

        obd_gate_diag_ack(diag);

        /* periodic DTC scan due-check: runs every iteration, incl. the
         * idle branch, so DTC works with polling disabled (dtc_enabled
         * without enabled; TASK_dtc.md §5). Voltage pause gates it: a
         * weak battery is no time for bus traffic; the client pause too. */
        if (!s_paused_voltage && !s_scan_pause && !s_paused_client &&
            !s_paused_diag)
        {
            ap_dtc_periodic_check();
        }

        /* Two kinds of rows (autopid_j1939.h): the chip's, which wait for
         * every pause above, and the passive ones (J1939 groups read from
         * the listener's store, nothing transmitted), which run through
         * them all. The voltage pause stops the passive rows only under
         * pause_mode "all": a weak battery is about bus traffic. */
        bool enabled = ap_core_enabled();
        bool chip_ok = enabled && !s_paused_voltage && !s_scan_pause &&
                       !s_paused_client && !s_paused_diag;
        bool passive_ok = enabled &&
                          !(s_paused_voltage && ap_settings_pause_all());

        /* bus guard: before the chip is pinned to a CAN protocol the native
         * controller listens to the bus. A request at the wrong bitrate
         * destroys the traffic of a live bus, and this task would repeat
         * it every period. Asked before every transmission until the
         * bitrate is proven (a bus asleep at boot wakes up later). */
        if (chip_ok)
        {
            s_paused_bus = !ap_guard_poll_ok();
            chip_ok = !s_paused_bus;
        }

        /* a '?' row reads the store like any passive row and, in j1939
           active mode, asks the network for its group first: not while a
           diagnostic tool holds the bus (its session is not ours to talk
           over), the reading goes on */
        unsigned classes =
            (chip_ok ? AP_CLASS_BIT(AP_CLASS_CHIP) : 0u) |
            (passive_ok ? (AP_CLASS_BIT(AP_CLASS_PASSIVE) |
                           AP_CLASS_BIT(AP_CLASS_BUS_TX)) : 0u);
        bool bus_tx_ok = passive_ok && !s_paused_diag;

        if (classes == 0)
        {
            stats->running = false;
            dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
            ulTaskNotifyTake(pdTRUE, AP_IDLE_WAIT);
            continue;
        }

        /* pick the next due entry under the lock; copy what we need */
        ap_core_lock();

        int64_t now = esp_timer_get_time();
        int64_t due = 0;
        int i = ap_sched_next_of(sched, cfg, classes, &due);
        int chip_i = chip_ok ? ap_sched_next_of(sched, cfg,
                                                AP_CLASS_BIT(AP_CLASS_CHIP),
                                                NULL)
                             : 0;
        ap_pid_t pid_copy;
        ap_filter_t flt_copy;
        ap_param_t *params_copy = s_params_copy;
        bool is_pid = false, is_filter = false;

        if (i >= 0 && due <= now && i < cfg->n_pids)
        {
            pid_copy = cfg->pids[i];
            memcpy(params_copy, &cfg->params[pid_copy.param_start],
                   sizeof(ap_param_t) * pid_copy.param_count);
            is_pid = true;
        }
        else if (i >= 0 && due <= now)
        {
            flt_copy = cfg->filters[i - cfg->n_pids];
            memcpy(params_copy, &cfg->params[flt_copy.param_start],
                   sizeof(ap_param_t) * flt_copy.param_count);
            is_filter = true;
        }

        ap_core_unlock();

        if (chip_i < 0)
        {
            /* the chip has no row to poll (empty tables, or a table of
               passive rows only): first contact's probe stands in for the
               poll, paced by itself */
            ap_runner_idle();
        }

        if (i < 0)
        {
            stats->running = false;
            dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
            ulTaskNotifyTake(pdTRUE, AP_IDLE_WAIT);
            continue;
        }

        stats->running = true;

        if (due > now)
        {
            int64_t wait_us = due - now;
            TickType_t ticks = pdMS_TO_TICKS((wait_us / 1000) + 1);

            dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
            ulTaskNotifyTake(pdTRUE,
                             (ticks > AP_IDLE_WAIT) ? AP_IDLE_WAIT : ticks);
            continue; /* re-evaluate: config/groups may have changed */
        }

        bool ok = true;
        bool passive = is_pid && ap_row_class(&pid_copy) != AP_CLASS_CHIP;

        if (passive)
        {
            /* the store, not the chip: no busy window (sleep may cut the
               chip's rail meanwhile), no bitrate proof, no identity. A
               message published counts as the vehicle answering. */
            uint32_t before = 0, after = 0;

            ap_runner_j1939_stats(&before, NULL);
            ok = ap_runner_run_j1939(&pid_copy, i, params_copy, bus_tx_ok);
            ap_runner_j1939_stats(&after, NULL);

            if (ok)
            {
                stats->passive_ok++;
            }
            else
            {
                stats->passive_failed++;
            }

            if (after != before)
            {
                s_last_ok_us = esp_timer_get_time();
                stats->last_poll_us = s_last_ok_us;
            }
        }
        else if (is_pid || is_filter)
        {
            /* Busy window: sleep_manager waits for AUTOPID_IDLE before
             * cutting the chip/rail (§ sleep prepare) */
            dev_status_manager_clear(DEV_STATUS_BIT_AUTOPID_IDLE);

            ok = is_pid ? ap_runner_run(&pid_copy, i, params_copy)
                        : ap_runner_run_filter(&flt_copy, params_copy);

            if (ok)
            {
                stats->polls_ok++;
                s_last_ok_us = esp_timer_get_time();
                ap_guard_proven(); /* an answer proves the bitrate */
            }
            else
            {
                stats->polls_failed++;
            }

            stats->last_poll_us = esp_timer_get_time();

            if (is_pid)
            {
                /* per-boot vehicle identity + stored-protocol fallback */
                ap_runner_poll_result(ok);
            }
        }

        char failed_name[AP_NAME_LEN + 12] = "";
        uint16_t streak = 0;

        ap_core_lock();
        ap_sched_ran(sched, cfg, i, esp_timer_get_time(), ok);

        if (!ok && (is_pid || is_filter))
        {
            streak = sched->slots[i].fail_streak;

            if (is_pid)
            {
                snprintf(failed_name, sizeof(failed_name), "%s",
                         pid_copy.name);
            }
            else
            {
                snprintf(failed_name, sizeof(failed_name), "filter %lX",
                         (unsigned long)flt_copy.frame_id);
            }
        }

        ap_core_unlock();

        if (failed_name[0] != '\0')
        {
            ap_events_pid_failed(failed_name, streak);
        }
    }
}

/* ---- what autopid.c and the chip jobs need from the task --------------------------- */

void ap_poller_start(QueueHandle_t batt_q)
{
    s_batt_q = batt_q;
    s_task = xTaskCreateStatic(poller_task, "autopid",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               5, s_stack, &s_tcb);
}

void ap_core_wake(void)
{
    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

void ap_core_scan_pause(bool on)
{
    s_scan_pause = on;

    if (!on)
    {
        /* the scan changed protocol/header state under the chip */
        ap_runner_reset();
    }

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

void ap_poller_pauses(autopid_stats_t *out)
{
    out->paused_voltage = s_paused_voltage;
    out->paused_client = s_paused_client;
    out->paused_diag = s_paused_diag;
    out->paused_bus = s_paused_bus;
}

int64_t ap_poller_last_ok_us(void)
{
    return s_last_ok_us;
}
