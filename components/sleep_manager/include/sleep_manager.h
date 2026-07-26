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
 * @file sleep_manager.h
 * @brief WiCAN low-power manager (feature component) — rewrite of
 *        legacy `sleep_mode.c` (TASK_sleep_manager.md).
 *
 * Model: battery voltage (from battery_monitor — this component owns
 * NO ADC) drives a pure policy ladder: NORMAL → LOW_VOLTAGE (below
 * sleep_voltage, a countdown of sleep_delay_min) → SLEEPING →
 * WAKE_PENDING (voltage recovered and stable) → **reboot**
 * (restart_tracker POWER_WAKE) rather than resume-in-place — every
 * manager restarts clean, the legacy-proven approach.
 *
 * SLEEPING = repeated LIGHT sleep with a 2 s timer wake (voltage still
 * sampled between naps; the timer wake doubles as meatpi's bench
 * failsafe — the device can never be stranded in a state only an
 * external signal can leave). Entry sequence: `sleep.entering` event →
 * wait for autopid idle → main's prepare callback (ordered component
 * stops — composition-root glue, so this component doesn't depend on
 * wifi/ble/mqtt/...) → CAN transceiver standby → OBD chip sleep
 * (verified each nap, ≤6 re-sleeps then recovery reboot) → USB power
 * rail held low.
 *
 * Boot-loop guard (legacy parity): ≥3 unexpected resets while the
 * battery reads below the error threshold forces sleep instead of
 * another crash lap.
 *
 * Bench-safety (meatpi 2026-07-07): the state task arms only after a
 * 15 s boot grace period (a bootloop still leaves a flash window),
 * and all sleeping is timer-woken light sleep — no deep sleep, no
 * wake source that can silently never fire.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SLEEP_MANAGER_NORMAL = 0,
    SLEEP_MANAGER_LOW_VOLTAGE,   /**< below threshold, countdown runs */
    SLEEP_MANAGER_SLEEPING,
    SLEEP_MANAGER_WAKE_PENDING,  /**< recovered, stability hold       */
} sleep_manager_state_t;

typedef struct
{
    bool                  enabled;
    sleep_manager_state_t state;
    float                 voltage;      /**< latest battery reading   */
    float                 sleep_v;      /**< boot-applied threshold   */
    float                 wake_v;       /**< sleep_v + 0.1 (legacy)   */
    uint32_t              naps;         /**< light-sleep cycles       */
    uint32_t              chip_resleeps;/**< OBD re-sleep retries     */
} sleep_manager_status_t;

/** Ordered shutdown glue the composition root provides (stop radios,
 *  loggers, tunnels... in main's dependency order). Runs in the sleep
 *  task right before the hardware is powered down. */
typedef void (*sleep_manager_prepare_cb_t)(void);

/** Register descriptors (settings/log/events). No hardware access. */
esp_err_t sleep_manager_init(void);

/** Create the state task when enabled (15 s boot grace first). */
esp_err_t sleep_manager_start(void);

/** Park the state task (no more sleep entries; never wakes hardware). */
esp_err_t sleep_manager_stop(void);

esp_err_t sleep_manager_status(sleep_manager_status_t *out);

/** Composition-root glue (ONE callback, main wires it). */
esp_err_t sleep_manager_set_prepare_cb(sleep_manager_prepare_cb_t cb);

/** Force a sleep entry for the bench: enters the full sequence, then
 *  auto-reboots after @p wake_after_s regardless of voltage (so a
 *  USB-powered bench unit always comes back). */
esp_err_t sleep_manager_test_sleep(uint32_t wake_after_s);

/** Optional /api/sleep route (§9.1; main wires it). */
esp_err_t sleep_manager_register_http(void);

/** `sleep` CLI; registered internally on the settings apply. */
esp_err_t sleep_manager_register_cli(void);

#ifdef __cplusplus
}
#endif
