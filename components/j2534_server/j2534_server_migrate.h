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
 * @file j2534_server_migrate.h
 * @brief The j2534_server settings migration (pure, cJSON only, host-tested):
 *        version 1 -> 2 (2026-10-27) drops `allow_reflash`, the ECU-flashing
 *        content gate that no longer exists.
 */
#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** settings_manager's on_migrate for "j2534_server": mutates @p settings in
 *  place from @p from_version to the current version. Every version ever
 *  shipped is handled: 1 -> 2 removes `allow_reflash`. */
esp_err_t j2534_settings_migrate(uint32_t from_version, cJSON *settings);

#ifdef __cplusplus
}
#endif
