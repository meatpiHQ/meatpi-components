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
 * @file rtc_manager_events.c
 * @brief event_manager glue: wall-clock pull values for rule templates
 *        (`${time.iso}` in MQTT payloads — main_events.c parity).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "event_manager.h"

#include "rtc_manager.h"

static esp_err_t read_time(const char *name, char *out, size_t len)
{
    if (strcmp(name, "time.iso") == 0)
    {
        return rtc_manager_now_iso8601(out, len);
    }

    if (strcmp(name, "time.epoch") == 0)
    {
        snprintf(out, len, "%lld", (long long)time(NULL));
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

void rtcm_events_register(void)
{
    (void)event_manager_register_value("time.iso", read_time);
    (void)event_manager_register_value("time.epoch", read_time);
}
