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
 * @file autopid_dtc_job.c
 * @brief DTC one-shot job task and its triggers (TASK_dtc.md §5): the
 *        scan start, the rule-queued clear and the periodic due-check
 *        the poller calls. Split out of autopid_dtc.c 2026-10-02
 *        (700-line rule); behaviour unchanged.
 *
 * The job task stack is PSRAM: DTC jobs never touch the filesystem
 * (the report is RAM-only by design; history = event rules -> logger).
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "autopid_private.h"
#include "autopid_dtc_engine.h"

static const char *TAG = "autopid";

static volatile bool s_busy;           /* a dtc job (scan or clear) runs  */
static int64_t s_last_scan_us;         /* periodic anchor (job START)     */

/* one-shot job task (scan, or a rule-queued clear) */
typedef enum
{
    DTC_JOB_SCAN = 0,
    DTC_JOB_CLEAR,
} dtc_job_t;

static dtc_job_t   s_job;
static char        s_job_codes[192];
static char        s_job_mode[12];
static StaticTask_t s_job_tcb;         /* internal: FreeRTOS object */
/* 16 KB (StackType_t = BYTES on xtensa): ap_resp_to_payload(s)' line
   table alone is ~5.6 KB of frame — 6144 overflowed the moment the
   scan grew the multi-ECU path (silent PSRAM-bss scribble; found
   2026-07-22 when an internal-RAM stack turned it into a panic) */
static StackType_t  s_job_stack[16384] EXT_RAM_BSS_ATTR; /* no fs I/O */

bool ap_dtc_busy(void)
{
    return s_busy;
}

/* ---- the job task + triggers -------------------------------------------------------- */

static void dtc_job_task(void *arg)
{
    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* one job at a time */

    (void)arg;

    if (s_job == DTC_JOB_SCAN)
    {
        ap_core_scan_pause(true);
        ap_dtc_run_scan(resp, sizeof(resp));
        ap_core_scan_pause(false);
        ap_core_job_release();
    }
    else
    {
        /* rule-queued clear: the job flag was NOT pre-acquired —
         * ap_dtc_clear() takes it itself; results travel by event */
        char err[48];

        (void)ap_dtc_clear(s_job_codes, s_job_mode, NULL, NULL, NULL,
                           err, sizeof(err));
    }

    /* ephemeral tasks escape System Monitor — surface the watermark so
       the stack-audit bench (and any log reader) sees how close this
       job came to the 2026-07-22 silent-overflow cliff */
    ESP_LOGI(TAG, "dtc job stack_hw=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t dtc_job_spawn(dtc_job_t job)
{
    s_job = job;

    if (xTaskCreateStatic(dtc_job_task, "apid_dtc",
                          sizeof(s_job_stack) / sizeof(s_job_stack[0]),
                          NULL, 5, s_job_stack, &s_job_tcb) == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ap_dtc_scan_start(void)
{
    if (!ap_dtc_cfg()->enabled)
    {
        return ESP_ERR_NOT_ALLOWED;
    }

    /* bus guard (the periodic check lands here too: while the guard
       parks the chip it looks at the bus again every 15 s at most) */
    if (!ap_guard_job_ok(NULL, 0) || !ap_dtc_obd_init_ok())
    {
        s_last_scan_us = esp_timer_get_time(); /* not every poller tick */
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_busy || !ap_core_job_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    s_last_scan_us = esp_timer_get_time();

    esp_err_t err = dtc_job_spawn(DTC_JOB_SCAN);

    if (err != ESP_OK)
    {
        s_busy = false;
        ap_core_job_release();
    }

    return err;
}

esp_err_t ap_dtc_clear_queue(const char *codes, const char *mode)
{
    if (!ap_dtc_cfg()->enabled || !ap_dtc_cfg()->allow_clear)
    {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (s_busy)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    snprintf(s_job_codes, sizeof(s_job_codes), "%s",
             (codes != NULL) ? codes : "");
    snprintf(s_job_mode, sizeof(s_job_mode), "%s",
             (mode != NULL) ? mode : "");

    esp_err_t err = dtc_job_spawn(DTC_JOB_CLEAR);

    if (err != ESP_OK)
    {
        s_busy = false;
    }

    return err;
}

void ap_dtc_periodic_check(void)
{
    if (!ap_dtc_cfg()->enabled || ap_dtc_cfg()->period_min == 0 || s_busy)
    {
        return;
    }

    int64_t period_us = (int64_t)ap_dtc_cfg()->period_min * 60 * 1000000;

    if (esp_timer_get_time() - s_last_scan_us >= period_us)
    {
        (void)ap_dtc_scan_start(); /* busy/collision = try next tick */
    }
}
