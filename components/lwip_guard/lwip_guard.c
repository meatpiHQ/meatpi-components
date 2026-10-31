/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file lwip_guard.c
 * @brief The link-time wrap of IDF's sys_thread_sem_get() (see
 *        include/lwip_guard.h for the model).
 */
#include "lwip_guard.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sys.h"

#include "log_manager.h"

#include "lwip_guard_private.h"

static const char *TAG = "lwip_guard";

/* internal: lwIP reads it as the task's StaticSemaphore_t; it stays all
   zero for the life of the firmware, which is lwIP's own "not a semaphore"
   (sys_sem_valid is a compare against zeros), so every caller that gets it
   answers ERR_MEM before touching it. Shared read-only by every refused
   task. */
static sys_sem_t s_none;
static lwip_guard_state_t s_state;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t lwip_guard_init(void)
{
    static const log_descriptor_t LOG = { "lwip_guard", ESP_LOG_INFO };

    (void)log_manager_register(&LOG);
    return ESP_OK;
}

uint32_t lwip_guard_refusals(void)
{
    return s_state.refusals;
}

/* IDF's getter: the calling task's semaphore, allocated (92 B, internal)
 * on the first call; NULL when that allocation fails. */
sys_sem_t *__real_sys_thread_sem_get(void);

sys_sem_t *__wrap_sys_thread_sem_get(void)
{
    sys_sem_t *sem = __real_sys_thread_sem_get();

    if (sem != NULL)
    {
        return sem;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool say;
    uint32_t refusals;

    portENTER_CRITICAL(&s_mux);
    say = lwip_guard_note(&s_state, now_ms);
    refusals = s_state.refusals;
    portEXIT_CRITICAL(&s_mux);

    if (say)
    {
        ESP_LOGE(TAG, "no internal RAM for the resolver semaphore of task %s "
                 "(%u B free): the call fails with ERR_MEM instead of a crash "
                 "(%lu refusals)",
                 pcTaskGetName(NULL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)refusals);
    }

    return &s_none;
}
