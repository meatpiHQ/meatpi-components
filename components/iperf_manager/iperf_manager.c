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
 * @file iperf_manager.c
 * @brief Lifecycle. The engine (managed espressif/iperf) owns its own
 *        tasks per session; stop aborts every instance.
 */
#include "iperf_manager.h"

#include "esp_log.h"
#include "iperf.h"

#include "log_manager.h"

#include "iperf_manager_private.h"

static const char *TAG = "iperf_manager";

esp_err_t iperf_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "iperf_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return ipm_settings_register();
}

esp_err_t iperf_manager_start(void)
{
    if (!ipm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    return ESP_OK; /* passive — sessions start from the CLI */
}

esp_err_t iperf_manager_stop(void)
{
    (void)iperf_stop_instance(IPERF_ALL_INSTANCES_ID);
    return ESP_OK;
}
