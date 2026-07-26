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

/** Internals shared across the can_manager .c files. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t can_manager_register_cli(void);

/* ---- settings (can_manager_settings.c) -------------------------------------- */

/** Register the "can_manager" descriptor with settings_manager. */
esp_err_t canm_settings_register(void);

/* boot-applied knobs; valid once canm_settings_is_configured() (§4.3) */
bool     canm_settings_enabled(void);
bool     canm_settings_silent(void);
uint32_t canm_settings_baud_kbps(void);
bool     canm_settings_is_configured(void);
