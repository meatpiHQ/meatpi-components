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
 * @file main.c
 * @brief Minimal meatpi-components consumer: settings + logging + CLI +
 *        CAN core + the bus-conversation gate + the add-on hook
 *        component (ext_manager — no-op unless a component-pack overlay
 *        provides an implementation).
 *
 * Boot follows the component standard: settings_manager_init() first,
 * component _init() calls register their settings, then
 * settings_manager_start() runs every on_apply exactly once.
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "can_manager.h"
#include "cmdline_manager.h"
#include "ext_manager.h"
#include "log_manager.h"
#include "obd_gate.h"
#include "settings_manager.h"

static const char *TAG = "example";

static void init_step(const char *name, esp_err_t err)
{
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s failed: %s", name, esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    init_step("nvs", err);

    init_step("log_manager", log_manager_init());
    init_step("settings_manager", settings_manager_init());
    init_step("cmdline_manager", cmdline_manager_init());
    init_step("can_manager", can_manager_init());
    init_step("obd_gate", obd_gate_init());
    init_step("ext_manager", ext_manager_init());

    init_step("settings_start", settings_manager_start());

    ESP_LOGI(TAG, "meatpi-components minimal example up");
}
