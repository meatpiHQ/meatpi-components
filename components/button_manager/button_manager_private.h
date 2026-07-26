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
 * @file button_manager_private.h
 * @brief Internal contracts: the PURE press-tracking state machine
 *        (host-testable, no GPIO/RTOS) and the settings surface.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure press tracking (button_manager_press.c) --------------------------- */

typedef struct
{
    uint32_t held_ticks; /* consecutive ticks the button has been down  */
    bool     fired;      /* long-press already reported for this press  */
} btn_press_t;

void btn_press_reset(btn_press_t *p);

/** Feed one poll tick. @return true exactly ONCE per press when
 *  @p pressed has been true for @p hold_ticks consecutive ticks;
 *  releasing re-arms. */
bool btn_press_step(btn_press_t *p, bool pressed, uint32_t hold_ticks);

/* ---- settings (button_manager_settings.c) ----------------------------------- */

esp_err_t btn_settings_register(void);
bool      btn_settings_enabled(void);
uint32_t  btn_settings_hold_s(void);
bool      btn_settings_is_configured(void);

#ifdef __cplusplus
}
#endif
