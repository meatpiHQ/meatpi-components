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
 * @file can_selftest_cli.c
 * @brief `can_selftest` (bare: the last result) and `can_selftest run` (the
 *        test in the console's task, about 2 s, then the same report).
 *        Standard §6b: registered from the settings apply behind `cli`.
 */
#include "can_selftest.h"

#include <string.h>

#include "esp_attr.h"
#include "cmdline_manager.h"

static can_selftest_result_t s_result EXT_RAM_BSS_ATTR;

static void print_result(const can_selftest_result_t *r)
{
    if (r->running)
    {
        cmdline_printf("running\n");
        return;
    }

    if (!r->done)
    {
        cmdline_printf("no run yet: can_selftest run\n");
        return;
    }

    if (r->refusal[0] != '\0')
    {
        cmdline_printf("not run: %s\n", r->refusal);
        return;
    }

    cmdline_printf("%s\n", r->verdict);
    cmdline_printf("bus:     %lu kbit/s, %s\n", (unsigned long)r->baud_kbps,
                   r->live ? "live (a vehicle talking)" : "silent");

    for (size_t i = 0; i < CAN_SELFTEST_STEPS; i++)
    {
        cmdline_printf("  [%s] %s (%lu ms): %s\n", r->steps[i].ok ? "ok" : "--",
                       r->steps[i].name, (unsigned long)r->steps[i].ms,
                       r->steps[i].detail);
    }

    for (size_t i = 0; i < r->n_lines; i++)
    {
        cmdline_printf("    %s\n", r->lines[i]);
    }

    cmdline_printf("took %lu ms\n", (unsigned long)r->took_ms);
}

static int cmd_can_selftest(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "run") == 0)
    {
        cmdline_printf("running the CAN self-test (about 2 s; the chip and "
                       "the bus are busy meanwhile):\n");

        esp_err_t err = can_selftest_run(&s_result);

        if (err == ESP_ERR_INVALID_STATE && s_result.refusal[0] == '\0')
        {
            cmdline_printf("a run is in progress\n");
            return 1;
        }

        print_result(&s_result);
        return s_result.pass ? 0 : 1;
    }

    if (argc > 1)
    {
        cmdline_printf("usage: can_selftest | can_selftest run\n");
        return 1;
    }

    can_selftest_last(&s_result);
    print_result(&s_result);
    return 0;
}

esp_err_t can_selftest_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "can_selftest",
        .help = "CAN self-test: the OBD chip and the ESP32's CAN controller "
                "exchange one OBD request over the shared bus",
        .hint = "Usage: can_selftest | can_selftest run",
        .func = cmd_can_selftest,
    };

    return cmdline_manager_register(&CMD);
}
