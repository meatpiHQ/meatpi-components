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
 * @file battery_monitor_cli.c
 * @brief The component's CLI command (`battery`) — registered into
 *        cmdline_manager by battery_monitor_register_cli() (main wires
 *        it in CLI compositions only, like battery_monitor_register_http).
 */
#include "cmdline_manager.h"

#include "battery_monitor.h"

static int cmd_battery(int argc, char **argv)
{
    float v = 0;
    esp_err_t err = battery_monitor_voltage(&v);

    (void)argc;
    (void)argv;

    if (err != ESP_OK)
    {
        cmdline_printf("no reading yet (%s)\n", esp_err_to_name(err));
        return 1;
    }

    cmdline_printf("battery: %.2f V\n", v);
    return 0;
}

esp_err_t battery_monitor_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "battery",
        .help = "Battery voltage status",
        .func = cmd_battery,
    };

    return cmdline_manager_register(&CMD);
}
