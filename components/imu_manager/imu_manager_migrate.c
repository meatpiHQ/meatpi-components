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
 * @file imu_manager_migrate.c
 * @brief PURE settings migration — cJSON only, host-testable.
 */
#include "cJSON.h"

#include "imu_manager_private.h"

esp_err_t imu_settings_migrate(uint32_t from_version, cJSON *settings)
{
    if (settings == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* v1 was WoM-only (wom_threshold), v2 SMD-only (smd_sensitivity),
     * v3 runs both. Every key either version carried is valid again in
     * v3, so both migrations are pass-throughs — validation fills the
     * missing keys (wom/smd enables etc.) from the schema defaults. */
    return (from_version >= 1) ? ESP_OK : ESP_ERR_INVALID_VERSION;
}
