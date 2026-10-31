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
 * @file http_server_manager_cli.c
 * @brief The `http` console command (2026-10-31): the server task's stack
 *        placement and the flash audit, the routes whose requests moved
 *        the SPI flash counters (`-a`: every route that served a request).
 *        Registered from the settings apply hook (there is no `cli`
 *        switch in this component's settings: the command is read-only
 *        diagnostics, and the console itself is behind the device's
 *        console settings).
 */
#include <string.h>

#include "cmdline_manager.h"

#include "http_server_manager.h"
#include "http_server_manager_private.h"

static int cmd_http(int argc, char **argv)
{
    bool all = (argc > 1 && strcmp(argv[1], "-a") == 0);
    size_t count = 0;
    const hsm_audit_row_t *rows = hsm_audit_rows(&count);
    uint32_t requests = 0;
    uint32_t served = 0;
    uint32_t touched = 0;

    cmdline_printf("stack: %s, %u B (the watermark: system -t)\n",
                   HSM_STACK_PSRAM ? "PSRAM" : "internal",
                   (unsigned)HSM_STACK_BYTES);
    cmdline_printf("%-7s %-36s %8s %7s %7s %7s\n", "method", "route",
                   "requests", "reads", "writes", "erases");

    for (size_t i = 0; i < count; i++)
    {
        const hsm_audit_row_t *r = &rows[i];

        if (r->requests == 0)
        {
            continue;
        }

        requests += r->requests;
        served++;

        bool flash = hsm_audit_row_touched_flash(r);

        if (flash)
        {
            touched++;
        }

        if (!all && !flash)
        {
            continue;
        }

        const httpd_uri_t *u = hsm_route_at(i);

        cmdline_printf("%-7s %-36s %8lu %7lu %7lu %7lu\n",
                       (u != NULL) ? http_method_str(u->method) : "GET",
                       (u != NULL) ? u->uri : "/* (the asset server)",
                       (unsigned long)r->requests,
                       (unsigned long)r->ops.reads,
                       (unsigned long)r->ops.writes,
                       (unsigned long)r->ops.erases);
    }

    cmdline_printf("%lu requests on %lu routes since boot, %lu routes moved "
                   "the flash counters%s\n",
                   (unsigned long)requests, (unsigned long)served,
                   (unsigned long)touched,
                   all ? "" : " (http -a lists every route served)");
    return 0;
}

esp_err_t hsm_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "http",
        .help = "HTTP server: the task's stack placement and the flash audit "
                "per route (-a: every route served)",
        .func = cmd_http,
    };

    return cmdline_manager_register(&CMD);
}
