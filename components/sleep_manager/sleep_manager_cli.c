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
 * @file sleep_manager_cli.c
 * @brief The `sleep` CLI command (§6b). `sleep test <secs>` forces a
 *        full sleep entry with a timed wake — the bench smoke.
 */
#include <stdlib.h>
#include <string.h>

#include "cmdline_manager.h"

#include "sleep_manager.h"

static const char *state_str(sleep_manager_state_t st)
{
    switch (st)
    {
        case SLEEP_MANAGER_LOW_VOLTAGE:  return "low voltage countdown";
        case SLEEP_MANAGER_SLEEPING:     return "sleeping";
        case SLEEP_MANAGER_WAKE_PENDING: return "wake pending";
        default:                         return "normal";
    }
}

static int cmd_sleep(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "test") == 0)
    {
        esp_err_t err = sleep_manager_test_sleep(
            (uint32_t)atoi(argv[2]));

        if (err != ESP_OK)
        {
            cmdline_printf("test sleep refused (%s) — enabled + "
                           "4..600 s\n", esp_err_to_name(err));
            return 1;
        }

        cmdline_printf("entering test sleep; the device reboots on "
                       "the wake timer\n");
        return 0;
    }

    sleep_manager_status_t st;

    (void)sleep_manager_status(&st);
    cmdline_printf("sleep: %s, state %s\n",
                   st.enabled ? "enabled" : "disabled",
                   state_str(st.state));
    cmdline_printf("  battery %.2f V (sleep < %.2f, wake >= %.2f)\n",
                   st.voltage, st.sleep_v, st.wake_v);
    cmdline_printf("  naps %lu, chip re-sleeps %lu\n",
                   (unsigned long)st.naps,
                   (unsigned long)st.chip_resleeps);
    return 0;
}

esp_err_t sleep_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "sleep",
        .help = "Sleep status; `sleep test <secs>` forces a timed "
                "test sleep",
        .func = cmd_sleep,
    };

    return cmdline_manager_register(&CMD);
}
