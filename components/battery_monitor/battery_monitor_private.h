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
 * @file battery_monitor_private.h
 * @brief Internal contracts: the PURE watch state machine (host-testable
 *        — time and voltage injected) and the ADC layer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ---- pure watch policy (battery_monitor_policy.c) -------------------------- */

#define BM_SIDE_UNKNOWN 0
#define BM_SIDE_ABOVE   1
#define BM_SIDE_BELOW   2

#define BM_EVAL_NONE  0
#define BM_EVAL_BELOW 1
#define BM_EVAL_ABOVE 2

typedef struct
{
    float    below_v;
    float    above_v;
    uint32_t hold_ms;
    int      side;          /* settled side (BM_SIDE_*)                 */
    int      pending;       /* side currently being debounced           */
    uint32_t pending_since;
} bm_watch_state_t;

/** false = malformed (above_v < below_v, negative hold irrelevant). */
bool bm_watch_init(bm_watch_state_t *w, float below_v, float above_v,
                   uint32_t hold_ms, uint32_t now_ms);

/** Feed one reading; returns BM_EVAL_NONE/BELOW/ABOVE. The first settled
 *  side after init is also reported (initial-state delivery). In the
 *  hysteresis band (below_v..above_v) nothing is pending. Wrap-safe. */
int bm_watch_eval(bm_watch_state_t *w, float voltage, uint32_t now_ms);

/* ---- ADC layer (battery_monitor_adc.c) --------------------------------------- */

esp_err_t bm_adc_init(void);
/** 8-sample averaged battery voltage in volts (divider + offset applied). */
esp_err_t bm_adc_read(float *out);

/* ---- settings (battery_monitor_settings.c) ---------------------------------- */

/** Register the "battery_monitor" descriptor with settings_manager. */
esp_err_t batt_settings_register(void);

bool     batt_settings_enabled(void);       /* component enabled at boot        */
uint32_t batt_settings_poll_ms(void);       /* sampler period                   */
bool     batt_settings_is_configured(void); /* boot apply ran (standard §4.3)   */

/* event_manager glue (battery_monitor_events.c) */
void bm_events_register(void);
void bm_events_start(void);
