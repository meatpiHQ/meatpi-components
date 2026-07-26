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
 * @file battery_monitor_events.c
 * @brief event_manager glue: the `battery.threshold {edge, volts}`
 *        source (from the existing watch API; the main_events.c
 *        product thresholds carried over) and the `battery.voltage`
 *        pull value.
 */
#include <stdio.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "event_manager.h"

#include "battery_monitor.h"

/* the demo/product thresholds (main_events.c parity): sagging under
   12.0 V = engine off + discharging; 12.5 V hysteresis = charging */
#define BM_EVT_BELOW_V 12.0f
#define BM_EVT_ABOVE_V 12.5f
#define BM_EVT_HOLD_MS 5000

static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;              /* internal: FreeRTOS object */
static uint8_t s_q_store[4 * sizeof(battery_monitor_event_t)]
    EXT_RAM_BSS_ATTR;
static TaskHandle_t s_task;
static StaticTask_t s_tcb;                 /* internal: FreeRTOS object */
static StackType_t s_stack[3584] EXT_RAM_BSS_ATTR; /* 2560 left exactly
                              512 B headroom (stack audit 2026-07-22)
                              — one dip from silent PSRAM corruption */

static void drain_task(void *arg)
{
    (void)arg;

    while (true)
    {
        battery_monitor_event_t be;

        if (xQueueReceive(s_q, &be, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        em_event_t ev = { 0 };

        snprintf(ev.source, sizeof(ev.source), "battery");
        snprintf(ev.name, sizeof(ev.name), "threshold");
        ev.kv[0] = em_kv_str("edge",
                             (be.type == BATTERY_MONITOR_EVENT_BELOW)
                                 ? "below" : "above");
        ev.kv[1] = em_kv_f64("volts", (double)be.voltage);
        ev.n = 2;
        (void)event_manager_publish(&ev);
    }
}

static esp_err_t read_voltage(const char *name, char *out, size_t len)
{
    (void)name;

    float volts = 0;

    if (battery_monitor_voltage(&volts) != ESP_OK)
    {
        return ESP_FAIL;
    }

    snprintf(out, len, "%.2f", (double)volts);
    return ESP_OK;
}

void bm_events_register(void)
{
    static const em_key_decl_t KEYS[] =
    {
        { "edge", EM_VAL_STR },
        { "volts", EM_VAL_F64 },
    };
    static const em_source_decl_t THRESHOLD =
    {
        .source = "battery", .name = "threshold",
        .description = "battery crossed 12.0 V down / 12.5 V up "
                       "(5 s hold; initial state delivered at boot)",
        .keys = KEYS, .n_keys = 2,
    };

    (void)event_manager_declare_source(&THRESHOLD);
    (void)event_manager_register_value("battery.voltage", read_voltage);
}

void bm_events_start(void)
{
    if (s_task != NULL)
    {
        return;
    }

    s_q = xQueueCreateStatic(4, sizeof(battery_monitor_event_t),
                             s_q_store, &s_q_buf);

    if (s_q == NULL ||
        battery_monitor_watch(&(battery_monitor_watch_cfg_t)
                              {
                                  .below_v = BM_EVT_BELOW_V,
                                  .above_v = BM_EVT_ABOVE_V,
                                  .hold_ms = BM_EVT_HOLD_MS,
                              }, s_q, NULL) != ESP_OK)
    {
        return;
    }

    s_task = xTaskCreateStatic(drain_task, "batt_events",
                               sizeof(s_stack) / sizeof(s_stack[0]),
                               NULL, 3, s_stack, &s_tcb);
}
