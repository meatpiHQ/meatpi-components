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
 * @file battery_monitor.c
 * @brief Lifecycle, the watch registry, and the sampler task (see
 *        include/battery_monitor.h for the model). Settings live in
 *        battery_monitor_settings.c (standard §4.1).
 */
#include "battery_monitor.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "battery_monitor_private.h"

static const char *TAG = "battery_monitor";

typedef struct
{
    bool             used;
    bm_watch_state_t state;
    QueueHandle_t    q;
} bm_watch_slot_t;

static bm_watch_slot_t s_watches[BATTERY_MONITOR_MAX_WATCHES]
    EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                            /* internal: FreeRTOS */
static StackType_t s_stack[4096] EXT_RAM_BSS_ATTR; /* 3072 left 768 B
                              headroom (stack audit 2026-07-22) — too
                              thin for a PSRAM stack, which corrupts
                              silently instead of panicking */
static volatile float s_voltage;
static volatile bool s_have_reading;
static uint32_t s_evt_drops;
static bool s_started;

/* ---- sampler task ------------------------------------------------------------ */

static void sampler_task(void *arg)
{
    (void)arg;

    while (true)
    {
        float v;

        if (bm_adc_read(&v) == ESP_OK)
        {
            s_voltage = v;
            s_have_reading = true;

            uint32_t now = esp_log_timestamp();

            ESP_LOGD(TAG, "%.2f V", v);
            xSemaphoreTake(s_lock, portMAX_DELAY);

            for (int i = 0; i < BATTERY_MONITOR_MAX_WATCHES; i++)
            {
                if (!s_watches[i].used)
                {
                    continue;
                }

                int r = bm_watch_eval(&s_watches[i].state, v, now);

                if (r == BM_EVAL_NONE)
                {
                    continue;
                }

                battery_monitor_event_t evt =
                {
                    .type = (r == BM_EVAL_BELOW)
                                ? BATTERY_MONITOR_EVENT_BELOW
                                : BATTERY_MONITOR_EVENT_ABOVE,
                    .watch_id = i,
                    .voltage = v,
                    .uptime_ms = now,
                };

                ESP_LOGI(TAG, "watch %d: %.2f V -> %s", i, v,
                         (r == BM_EVAL_BELOW) ? "BELOW" : "ABOVE");

                if (xQueueSend(s_watches[i].q, &evt, 0) != pdTRUE)
                {
                    s_evt_drops++;

                    if ((s_evt_drops % 100) == 1)
                    {
                        ESP_LOGW(TAG, "subscriber full (%lu drops)",
                                 (unsigned long)s_evt_drops);
                    }
                }
            }

            xSemaphoreGive(s_lock);
        }
        else
        {
            /* rate-limited WARN, not LOGD: a silently-failing ADC
             * freezes the cached voltage and every consumer downstream
             * (the sleep wake path chased exactly that 2026-07-21) */
            static uint32_t s_fails;

            s_fails++;

            if (s_fails <= 3 || (s_fails % 1000) == 0)
            {
                ESP_LOGW(TAG, "ADC read failed (#%lu)",
                         (unsigned long)s_fails);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(batt_settings_poll_ms()));
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t battery_monitor_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "battery_monitor", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    bm_events_register();
    return batt_settings_register();
}

esp_err_t battery_monitor_start(void)
{
    if (!batt_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (!batt_settings_enabled())
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    esp_err_t err = bm_adc_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* one synchronous reading BEFORE the sampler exists (concurrent
     * adc_oneshot_read calls fail on the unit lock) so the boot log
     * shows the supply */
    float v;

    if (bm_adc_read(&v) == ESP_OK)
    {
        s_voltage = v;
        s_have_reading = true;
        ESP_LOGI(TAG, "started (%.2f V, poll %lus, ch%d, div x%.2f)", v,
                 (unsigned long)(batt_settings_poll_ms() / 1000),
                 CONFIG_WICAN_BATT_ADC_CHANNEL,
                 (float)CONFIG_WICAN_BATT_DIVIDER_X100 / 100.0f);
    }
    else
    {
        ESP_LOGW(TAG, "started (first reading failed; poll %lus)",
                 (unsigned long)(batt_settings_poll_ms() / 1000));
    }

    s_task = xTaskCreateStatic(sampler_task, "batt_sampler",
                               sizeof(s_stack) / sizeof(s_stack[0]),
                               NULL, 4, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_FAIL;
    }

    s_started = true;
    bm_events_start();
    return ESP_OK;
}

esp_err_t battery_monitor_stop(void)
{
    s_started = false; /* sampler keeps its schedule; readings continue */
    return ESP_OK;
}

/* ---- public API --------------------------------------------------------------- */

esp_err_t battery_monitor_voltage(float *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_have_reading)
    {
        return ESP_ERR_INVALID_STATE;
    }

    *out = s_voltage;
    return ESP_OK;
}

esp_err_t battery_monitor_read_now(float *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    float v;
    esp_err_t err = bm_adc_read(&v);

    if (err != ESP_OK)
    {
        return err;
    }

    s_voltage = v;
    s_have_reading = true;
    *out = v;
    return ESP_OK;
}

esp_err_t battery_monitor_watch(const battery_monitor_watch_cfg_t *cfg,
                                QueueHandle_t q, int *out_id)
{
    if (cfg == NULL || q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < BATTERY_MONITOR_MAX_WATCHES; i++)
    {
        if (s_watches[i].used)
        {
            continue;
        }

        if (!bm_watch_init(&s_watches[i].state, cfg->below_v,
                           cfg->above_v, cfg->hold_ms,
                           esp_log_timestamp()))
        {
            err = ESP_ERR_INVALID_ARG;
            break;
        }

        s_watches[i].q = q;
        s_watches[i].used = true;

        if (out_id != NULL)
        {
            *out_id = i;
        }

        err = ESP_OK;
        break;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t battery_monitor_unwatch(int watch_id)
{
    if (watch_id < 0 || watch_id >= BATTERY_MONITOR_MAX_WATCHES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = s_watches[watch_id].used ? ESP_OK : ESP_ERR_NOT_FOUND;

    s_watches[watch_id].used = false;
    xSemaphoreGive(s_lock);
    return err;
}
