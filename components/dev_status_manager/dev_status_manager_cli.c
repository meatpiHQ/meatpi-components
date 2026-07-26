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
 * @file dev_status_manager_cli.c
 * @brief The component's CLI commands (`version`, `status`) —
 *        registered into cmdline_manager by
 *        dev_status_manager_register_cli() (main wires it in CLI
 *        compositions only, like the *_register_http pattern).
 */
#include <string.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"

#include "cmdline_manager.h"

#include "dev_status_manager.h"

#ifndef CONFIG_WICAN_HW_VERSION
#define CONFIG_WICAN_HW_VERSION "unknown"
#endif

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();

    (void)argc;
    (void)argv;
    cmdline_printf("firmware:  %s\n", dev_status_manager_app_version());
    cmdline_printf("hardware:  %s\n", CONFIG_WICAN_HW_VERSION);
    cmdline_printf("device_id: %s\n", dev_status_manager_device_id());
    cmdline_printf("partition: %s\n",
                   dev_status_manager_partition_label());
    cmdline_printf("build:     %s %s\n", app->date, app->time);
    cmdline_printf("idf:       %s\n", esp_get_idf_version());
    cmdline_printf("OK\n"); /* legacy version trailer */
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    char buf[32];
    EventBits_t bits = dev_status_manager_get();

    (void)argc;
    (void)argv;

    if (dev_status_manager_format_uptime(buf, sizeof(buf)) > 0)
    {
        cmdline_printf("uptime: %s\n", buf);
    }

    cmdline_printf("status: 0x%06x\n", (unsigned)bits);

    for (int i = 0; i < 24; i++)
    {
        EventBits_t bit = (EventBits_t)1 << i;

        if ((bits & bit) != 0)
        {
            cmdline_printf("  %s\n", dev_status_manager_bit_name(bit));
        }
    }

    return 0;
}

static int cmd_faults(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "-c") == 0 ||
                     strcmp(argv[1], "--clear") == 0))
    {
        cmdline_printf("%s\n",
                       (dev_status_manager_faults_clear() == ESP_OK)
                           ? "OK" : "Error: clear failed");
        return 0;
    }

    static dev_status_fault_t faults[DEV_STATUS_FAULT_MAX]; /* off-stack */
    int n = dev_status_manager_faults(faults, DEV_STATUS_FAULT_MAX);

    if (n == 0)
    {
        cmdline_printf("no fault codes\nOK\n");
        return 0;
    }

    for (int i = 0; i < n; i++)
    {
        cmdline_printf("%-22s x%-4lu %s\n", faults[i].code,
                       (unsigned long)faults[i].count, faults[i].detail);
    }

    cmdline_printf("%d fault code(s) — clear with `faults -c`\nOK\n", n);
    return 0;
}

esp_err_t dev_status_manager_register_cli(void)
{
    static const esp_console_cmd_t CMDS[] =
    {
        { .command = "version", .help = "Get firmware version",
          .func = cmd_version },
        { .command = "status", .help = "Get system status",
          .func = cmd_status },
        { .command = "faults",
          .help = "Latched device fault codes (like DTCs); -c/--clear",
          .func = cmd_faults },
    };

    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++)
    {
        esp_err_t err = cmdline_manager_register(&CMDS[i]);

        if (err != ESP_OK)
        {
            return err;
        }
    }

    return ESP_OK;
}
