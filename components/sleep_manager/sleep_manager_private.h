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
 * @file sleep_manager_private.h
 * @brief Internals: the PURE policy ladder (host-tested) + glue hooks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sleep_manager.h"

/* legacy-carried constants */
#define SM_WAKE_DELTA_V     0.1f     /* wake_v = sleep_v + this        */
#define SM_CRITICAL_V       11.90f   /* below: no periodic wakeups     */
#define SM_ERROR_V          12.10f   /* boot-loop guard voltage gate   */
#define SM_WAKE_STABLE_MS   1000u    /* recovery must hold this long   */
#define SM_BOOT_GRACE_MS    15000u   /* meatpi: flash window on loops  */
#define SM_NAP_US           (2u * 1000u * 1000u)
#define SM_RESLEEP_MAX      6        /* OBD re-sleeps before recovery  */
#define SM_AUTOPID_IDLE_MS  20000u

/* ---- sleep_manager_policy.c — PURE (host-tested) -------------------------- */

typedef enum
{
    SM_ACT_NONE = 0,
    SM_ACT_ENTER_SLEEP,     /* run the shutdown sequence, start napping */
    SM_ACT_WAKE_REBOOT,     /* voltage recovered: POWER_WAKE reboot     */
    SM_ACT_PERIODIC_WAKE,   /* periodic check-in: POWER_WAKE reboot     */
} sm_action_t;

typedef struct
{
    float    sleep_v;
    float    wake_v;
    uint32_t delay_ms;       /* LOW_VOLTAGE countdown                  */
    bool     periodic;
    uint32_t interval_ms;    /* periodic check-in while SLEEPING       */
} sm_cfg_t;

typedef struct
{
    sleep_manager_state_t state;
    uint32_t              t_low;      /* LOW_VOLTAGE deadline          */
    uint32_t              t_stable;   /* WAKE_PENDING deadline         */
    uint32_t              t_periodic; /* next periodic check-in        */
} sm_policy_t;

void sm_policy_init(sm_policy_t *p);

/* one evaluation step; timestamps are ms and may wrap (deadline math
 * is subtraction-based) */
sm_action_t sm_policy_eval(sm_policy_t *p, const sm_cfg_t *cfg,
                           float volts, uint32_t now_ms);

/* ---- settings (sleep_manager_settings.c) -----------------------------------
 * `sleep_` prefix, not `sm_`: socket_manager already links sm_settings_*. */

/** Register the "sleep_manager" descriptor with settings_manager. */
esp_err_t sleep_settings_register(void);

/** Boot-applied config; valid once sleep_settings_is_configured(). */
const sm_cfg_t *sleep_settings_config(void);
bool sleep_settings_enabled(void);
bool sleep_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/* ---- glue ----------------------------------------------------------------- */

void sm_events_register(void);
void sm_events_entering(float volts);           /* BEFORE teardown */
void sm_events_state(const char *state, float volts);
