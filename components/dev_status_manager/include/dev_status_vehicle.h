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
 * @file dev_status_vehicle.h
 * @brief The current vehicle summary (2026-10-26): which car the live
 *        tables belong to and whether its setup ever finished, for the web
 *        UI's notice bar and the dashboard on the status poll they make
 *        anyway (`vehicle` on GET /api/status; absent while the store has
 *        no current car). Born from Ali's report of the day: a device set
 *        up on one car (the simulator) and plugged into another met a new
 *        car, which became current with its rows stored off, and the
 *        dashboard went empty without a word. The composition root fills
 *        it from autopid (dev_status_manager must not know the feature
 *        components).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char    name[32];   /**< the car's name as the store shows it          */
    char    key[24];    /**< its key: the VIN, or `fp:<8 hex>` without one  */
    char    vin[18];    /**< "" when no VIN answered                        */
    bool    pending;    /**< its Quick Setup never finished (the store's
                             `pending_profile`): nothing chosen for it yet  */
    uint8_t cars;       /**< how many cars the store holds, this one in     */
} dev_status_vehicle_t;

/** Fill @p out; false = no current car (no `vehicle` object). */
typedef bool (*dev_status_vehicle_provider_t)(dev_status_vehicle_t *out);

#ifdef __cplusplus
}
#endif
