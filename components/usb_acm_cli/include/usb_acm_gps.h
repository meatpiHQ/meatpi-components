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
 * @file usb_acm_gps.h
 * @brief ESPNetLink GPS fix — the dongle's `gps -p -j` JSON parsed into a
 *        flat struct. Pure, no deps → host-testable. The values are
 *        published as first-class autopid parameters (gps_*), so they ride
 *        the same cache / snapshot / value-sink / event path as polled
 *        PIDs and reach everywhere autopid data goes (HA push, data_logger,
 *        dashboard, event rules).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A GPS fix in the dongle's native units (only a LIVE fix is reported —
 *  a stale/cached AGNSS position is never presented as current). */
typedef struct
{
    bool     valid;        /**< A live fix is present (`"valid":true`).     */
    double   latitude;     /**< Decimal degrees, -90..90.                   */
    double   longitude;    /**< Decimal degrees, -180..180.                 */
    double   altitude_m;   /**< Metres above sea level.                     */
    double   speed_kmph;   /**< Ground speed, km/h.                         */
    double   heading_deg;  /**< Course over ground, 0..360.                 */
    int      satellites;   /**< Satellites used in the fix.                 */
    int      accuracy_m;   /**< Horizontal accuracy estimate, m (HDOP×5).   */
    uint32_t age_ms;       /**< Age of the fix (filled by the cache getter).*/
} usb_acm_gps_t;

/**
 * Parse the dongle's `gps -p -j` response. The JSON object may be wrapped
 * in the console echo/prompt — the first `{`…`}` is used. `heading_deg`
 * comes from `course_deg`; `accuracy_m` = round(`hdop`×5).
 *
 * @return true and fills @p out from a LIVE fix when `"valid":true`;
 *         false (and `out` zeroed) otherwise.
 */
bool usb_acm_gps_parse(const char *json, usb_acm_gps_t *out);

#ifdef __cplusplus
}
#endif
