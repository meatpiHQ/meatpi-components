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
 * @file ext_manager.c
 * @brief Stock no-op add-on hooks (see ext_manager.h). A component pack
 *        overlays this component with its own implementation.
 */
#include "ext_manager.h"

esp_err_t ext_manager_init(void)
{
    return ESP_OK;
}

esp_err_t ext_manager_start(void)
{
    return ESP_OK;
}

esp_err_t ext_manager_stop(void)
{
    return ESP_OK;
}
