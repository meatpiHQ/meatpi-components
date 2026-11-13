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
 * @file j2534_server_voltage.c
 * @brief READ_VBATT (IOCTL 0x03): the battery monitor's latest averaged
 *        reading as rounded millivolts. Device-wide, no channel and no
 *        bus transaction. A closed device, a missing sample and a reading
 *        that is not a plausible voltage each answer an error and leave
 *        the output untouched (J2534_WIRE_PROTOCOL.md section 3).
 *        Host-tested against a fake battery owner (host_test/test_voltage.c).
 */
#include "j2534_server_private.h"

#include <math.h>

#include "esp_log.h"

#include "battery_monitor.h"

static const char *TAG = "j2534_server";

uint32_t j2534_srv_read_vbatt(bool device_open, uint32_t *out)
{
    if (!device_open)
    {
        return J2534_ERR_DEVICE_NOT_CONNECTED;
    }

    float voltage;
    esp_err_t err = battery_monitor_voltage(&voltage);
    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "READ_VBATT unavailable: %s", esp_err_to_name(err));
        return J2534_ERR_FAILED;
    }

    double millivolts = (double)voltage * 1000.0 + 0.5;
    if (!isfinite(voltage) || voltage < 0.0f || millivolts > UINT32_MAX)
    {
        ESP_LOGW(TAG, "READ_VBATT invalid battery reading");
        return J2534_ERR_FAILED;
    }

    *out = (uint32_t)millivolts;
    return J2534_STATUS_NOERROR;
}
