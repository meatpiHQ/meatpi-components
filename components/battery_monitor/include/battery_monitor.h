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
 * @file battery_monitor.h
 * @brief WiCAN battery/supply voltage owner (feature component).
 *
 * Owns the battery-sense ADC input (WiCAN Pro: ADC1 CH3 = GPIO4 behind a
 * ÷11 divider, 6 dB attenuation — Kconfig per hardware revision) and
 * turns it into ONE product-level signal: the supply voltage, sampled
 * every `poll_s`, 8-sample averaged, calibration-corrected.
 *
 * WATCHES: any component registers a threshold pair + debounce and
 * subscribes a queue — cross below `below_v` (held for `hold_ms`) →
 * BELOW event; recover to `above_v` (the hysteresis gap prevents
 * flapping around one threshold) held for `hold_ms` → ABOVE event. The
 * first stable side after registration is also delivered, so consumers
 * learn the current state without polling. This is the foundation the
 * future sleep_manager builds on (its sleep/wake voltages become one
 * watch), and anything else can react too — stop ECU polling, send an
 * MQTT alert, log a voltage sag.
 *
 * Fan-out semantics match imu_manager: zero-timeout queue sends,
 * drop-and-count, the sampler task never blocks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BATTERY_MONITOR_MAX_WATCHES 8

typedef enum
{
    BATTERY_MONITOR_EVENT_BELOW = 0, /* sustained under below_v          */
    BATTERY_MONITOR_EVENT_ABOVE,     /* sustained at/over above_v        */
} battery_monitor_event_type_t;

typedef struct
{
    battery_monitor_event_type_t type;
    int      watch_id;   /* from battery_monitor_watch()                */
    float    voltage;    /* the reading that completed the debounce     */
    uint32_t uptime_ms;
} battery_monitor_event_t;

typedef struct
{
    float    below_v;    /* BELOW when sustained under this             */
    float    above_v;    /* ABOVE when sustained at/over this (must be  */
                         /* >= below_v — the hysteresis gap)            */
    uint32_t hold_ms;    /* debounce: how long a side must persist      */
} battery_monitor_watch_cfg_t;

/** Register settings ("battery_monitor") + log descriptors. No ADC. */
esp_err_t battery_monitor_init(void);

/** Bring up the ADC (+ calibration) and start the sampler task.
 *  ESP_ERR_INVALID_STATE when unconfigured (§4.3 step 5). */
esp_err_t battery_monitor_start(void);
esp_err_t battery_monitor_stop(void);

/** Latest averaged reading in volts. ESP_ERR_INVALID_STATE before the
 *  first successful sample. */
esp_err_t battery_monitor_voltage(float *out);

/**
 * Fresh synchronous ADC read (also refreshes the cached value). For
 * callers that cannot rely on the sampler task's cadence — the
 * light-sleep nap loop advances the RTOS tick ~40x slower than wall
 * time, so the cache goes minutes stale there (2026-07-21 wake bug).
 * May race a concurrent sampler cycle: one of the two reads can fail
 * that round (adc_oneshot unit lock) — retry-tolerant by design.
 */
esp_err_t battery_monitor_read_now(float *out);

/**
 * Register a threshold watch delivering battery_monitor_event_t into
 * @p q (caller-owned). Returns the watch id via @p out_id (nullable) —
 * events carry it so one queue can serve several watches.
 * ESP_ERR_NO_MEM when the table is full; ESP_ERR_INVALID_ARG for a
 * malformed cfg (above_v < below_v, zero queue).
 */
esp_err_t battery_monitor_watch(const battery_monitor_watch_cfg_t *cfg,
                                QueueHandle_t q, int *out_id);

/** Remove a watch by id. */
esp_err_t battery_monitor_unwatch(int watch_id);

/** Register GET /api/battery (composition root calls it in HTTP
 *  builds). */
esp_err_t battery_monitor_register_http(void);

/** Register the `battery` CLI command with cmdline_manager. Called
 *  INTERNALLY on the settings boot apply when the `cli` setting is true
 *  (default) — main no longer wires it. */
esp_err_t battery_monitor_register_cli(void);

#ifdef __cplusplus
}
#endif
