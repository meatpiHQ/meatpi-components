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
 * @file button_manager.h
 * @brief The hardware button (GPIO8, active low) as a component.
 *
 * Two product behaviors ride this pin (meatpi 2026-07-19):
 *
 *  - HELD AT POWER-ON  -> SAFE MODE. That check runs BEFORE any
 *    component init (bad settings must not be loadable), so it lives in
 *    the composition root (main/main_safemode.c), NOT here. This
 *    component owns the pin only from _start() onwards.
 *
 *  - LONG-PRESS WHILE RUNNING -> CONFIG MODE. This component detects
 *    the hold (settings-tunable, default 5 s at a 1 s tick — the legacy
 *    config_mode.c cadence) and fires the registered callback ONCE per
 *    press. WHAT config mode does (force AP, stop BLE, LED pattern,
 *    timeout-reboot) is composition policy wired by main
 *    (main_glue_wire_button) — this component knows no other component.
 *
 * Settings ("button_manager", v1): {enabled (true), hold_s (1..30, 5)}.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_MANAGER_GPIO 8

/** Fired from the button task (small INTERNAL stack — the callback may
 *  reach radio/flash paths, §2) once per press when the hold threshold
 *  is crossed. Re-arms after release. */
typedef void (*button_manager_longpress_cb_t)(void);

esp_err_t button_manager_init(void);   /* settings + log registration    */
esp_err_t button_manager_start(void);  /* GPIO + the 1 s poll task       */
esp_err_t button_manager_stop(void);

esp_err_t button_manager_set_longpress_cb(button_manager_longpress_cb_t cb);

#ifdef __cplusplus
}
#endif
