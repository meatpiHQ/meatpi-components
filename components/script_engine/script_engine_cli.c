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
 * @file script_engine_cli.c
 * @brief `script` console command: `script stop` kills a running script;
 *        `script test` runs a tiny built-in Berry snippet (proof of life).
 *        Arbitrary source is best driven via POST /api/scripts/run.
 */
#include <string.h>

#include "cmdline_manager.h"

#include "script_engine.h"
#include "script_engine_private.h"

static int cmd_script(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stop") == 0)
    {
        script_engine_kill();
        cmdline_printf("kill requested\nOK\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "run") == 0)
    {
        static char out[1024];

        esp_err_t err = script_engine_run_file(argv[2], out, sizeof(out));

        if (err == ESP_ERR_NOT_FOUND)
        {
            cmdline_printf("no such script in " SE_SCRIPTS_DIR "\n");
            return 1;
        }

        cmdline_printf("%s\n%s\n", out, err == ESP_OK ? "OK" : "ERROR");
        return err == ESP_OK ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "test") == 0)
    {
        static char out[512];
        const char *src =
            "log('berry alive')\n"
            "var s = 0\n"
            "for i : 1..10 s = s + i end\n"
            "log('sum 1..10 = ' + str(s))\n";

        esp_err_t err = script_engine_run(src, out, sizeof(out));
        cmdline_printf("%s\n%s\n", out, err == ESP_OK ? "OK" : "ERROR");
        return err == ESP_OK ? 0 : 1;
    }

    cmdline_printf("script busy=%s\n",
                   script_engine_busy() ? "yes" : "no");
    cmdline_printf("usage: script run <name> | test | stop\n");
    cmdline_printf("(run source via POST /api/scripts/run)\nOK\n");
    return 0;
}

esp_err_t script_engine_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "script",
        .help = "Berry scripting: script run <name> | test | stop",
        .func = cmd_script,
    };

    return cmdline_manager_register(&CMD);
}
