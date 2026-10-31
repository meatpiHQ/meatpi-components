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
 * @file flash_jobs.c
 * @brief The worker task and the mailbox (target side). The lifetime
 *        decisions are flash_jobs_core.c's, taken under s_mux; callers are
 *        serialized by s_lock, so one job is published at a time and the
 *        job's argument stays valid until its caller returns.
 */
#include "flash_jobs.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "flash_jobs_private.h"

static const char *TAG = "flash_jobs";

/* the worker runs at least here, and one above its caller (a job never
   waits behind the task that is waiting for it) */
#define FJ_PRIO_MIN 6

static SemaphoreHandle_t s_lock;     /* callers queue here                  */
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object           */
static SemaphoreHandle_t s_wake;     /* caller -> worker                    */
static StaticSemaphore_t s_wake_buf; /* internal: FreeRTOS object           */
static SemaphoreHandle_t s_done;     /* worker -> caller                    */
static StaticSemaphore_t s_done_buf; /* internal: FreeRTOS object           */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;

static fj_state_t             s_st;     /* under s_mux                      */
static TaskHandle_t           s_worker; /* under s_mux                      */
static flash_job_fn_t         s_fn;     /* the published job (s_lock held)  */
static void                  *s_arg;
static volatile esp_err_t     s_err;
static bool                   s_inited;

bool flash_jobs_on_internal_stack(void)
{
    int probe = 0; /* only its ADDRESS matters: where this stack lives */

    return esp_ptr_internal(&probe);
}

esp_err_t flash_jobs_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG = { "flash_jobs", ESP_LOG_INFO };

    (void)log_manager_register(&LOG);

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_wake = xSemaphoreCreateBinaryStatic(&s_wake_buf);
    s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    memset(&s_st, 0, sizeof(s_st));
    s_inited = (s_lock != NULL && s_wake != NULL && s_done != NULL);
    return s_inited ? ESP_OK : ESP_FAIL;
}

static void worker_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        bool signalled =
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(FLASH_JOBS_IDLE_MS)) == pdTRUE;

        portENTER_CRITICAL(&s_mux);

        fj_worker_step_t step = fj_worker_wake(&s_st, signalled);
        flash_job_fn_t   fn = s_fn;
        void            *a = s_arg;

        if (step == FJ_WORKER_EXIT)
        {
            s_worker = NULL; /* the next caller creates the next worker */
        }

        portEXIT_CRITICAL(&s_mux);

        if (step == FJ_WORKER_EXIT)
        {
            break;
        }

        if (step == FJ_WORKER_WAIT)
        {
            continue; /* a stale signal: nothing published */
        }

        s_err = fn(a);
        xSemaphoreGive(s_done);
    }

    /* the ephemeral-task rule (standard, section 2): the watermark goes
       out before the task is gone, so the stack audit can see it */
    uint32_t hw = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    portENTER_CRITICAL(&s_mux);
    fj_worker_exit(&s_st, hw);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGD(TAG, "worker idle, leaving: stack_hw=%lu", (unsigned long)hw);
    vTaskDelete(NULL);
}

/* Create the worker (internal heap: the stack must survive the cache-off
   windows) and record the outcome. Called with s_lock held. */
static esp_err_t spawn_worker(void)
{
    TaskHandle_t h = NULL;
    BaseType_t   ok = xTaskCreate(worker_task, "flash_jobs",
                                  FLASH_JOBS_STACK_BYTES, NULL, FJ_PRIO_MIN,
                                  &h);

    portENTER_CRITICAL(&s_mux);
    fj_spawned(&s_st, ok == pdPASS);

    if (ok == pdPASS)
    {
        s_worker = h;
    }

    portEXIT_CRITICAL(&s_mux);

    if (ok == pdPASS)
    {
        return ESP_OK;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool     say;

    portENTER_CRITICAL(&s_mux);
    say = fj_refused(&s_st, now_ms);
    portEXIT_CRITICAL(&s_mux);

    if (say)
    {
        /* W: the caller gets ESP_ERR_NO_MEM and says what it could not do;
           the device is out of internal RAM, which the health net sees */
        ESP_LOGW(TAG, "no internal RAM for the flash worker (%u B stack; "
                      "largest free block %u B): job refused",
                 (unsigned)FLASH_JOBS_STACK_BYTES,
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t flash_jobs_run(flash_job_fn_t fn, void *arg)
{
    if (fn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    fj_route_t how = fj_route(flash_jobs_on_internal_stack());

    if (how == FJ_INLINE)
    {
        esp_err_t err = fn(arg);

        if (s_inited)
        {
            portENTER_CRITICAL(&s_mux);
            fj_done(&s_st, FJ_INLINE);
            portEXIT_CRITICAL(&s_mux);
        }

        return err;
    }

    if (!s_inited)
    {
        ESP_LOGW(TAG, "flash work from a PSRAM stack before init: refused");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_fn = fn;
    s_arg = arg;
    s_err = ESP_FAIL;

    portENTER_CRITICAL(&s_mux);

    bool         need_worker = fj_publish(&s_st);
    TaskHandle_t worker = s_worker;

    portEXIT_CRITICAL(&s_mux);

    if (need_worker)
    {
        esp_err_t err = spawn_worker();

        if (err != ESP_OK)
        {
            xSemaphoreGive(s_lock);
            return err;
        }

        portENTER_CRITICAL(&s_mux);
        worker = s_worker;
        portEXIT_CRITICAL(&s_mux);
    }

    /* the worker never waits behind its own caller: at least one above */
    UBaseType_t mine = uxTaskPriorityGet(NULL);

    if (worker != NULL && mine + 1 < configMAX_PRIORITIES &&
        uxTaskPriorityGet(worker) <= mine)
    {
        vTaskPrioritySet(worker, mine + 1);
    }

    xSemaphoreGive(s_wake);
    xSemaphoreTake(s_done, portMAX_DELAY);

    esp_err_t err = s_err;

    portENTER_CRITICAL(&s_mux);
    fj_done(&s_st, FJ_MARSHAL);
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_lock);
    return err;
}

void flash_jobs_stats(flash_jobs_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    out->inline_runs = s_st.inline_runs;
    out->marshalled = s_st.marshalled;
    out->spawns = s_st.spawns;
    out->exits = s_st.exits;
    out->refusals = s_st.refusals;
    out->stack_hw_min = s_st.stack_hw_min;
    out->worker_alive = s_st.worker_alive;
    portEXIT_CRITICAL(&s_mux);
}
