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
 * @file can_manager_cli.c
 * @brief The `can` console command (§6b self-registered via settings).
 */
#include <string.h>

#include "cmdline_manager.h"

#include "can_manager.h"
#include "can_manager_private.h"

#include "can_core_filter.h"

static const char *bus_state_str(uint8_t state)
{
    switch (state)
    {
        case CAN_CORE_BUS_RUNNING:    return "running";
        case CAN_CORE_BUS_OFF:        return "BUS-OFF";
        case CAN_CORE_BUS_RECOVERING: return "recovering";
        default:                        return "stopped";
    }
}

/* can send <id> [hexbytes] [-r] — TX one frame (id decides 11/29-bit) */
static int cmd_can_send(int argc, char **argv)
{
    uint32_t id = 0;
    bool ext = false;
    bool rtr = false;
    uint8_t data[8];
    size_t dlc = 0;
    const char *payload = NULL;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0)
        {
            rtr = true;
        }
        else if (payload == NULL && i > 2)
        {
            payload = argv[i];
        }
    }

    if (argc < 3 || !can_core_parse_id(argv[2], &id, &ext))
    {
        cmdline_printf("usage: can send <id> [hexbytes] [-r]\n"
                       "       can send 7AA 11223344\nERROR\n");
        return 1;
    }

    if (payload != NULL
        && !can_core_parse_bytes(payload, data, sizeof(data), &dlc))
    {
        cmdline_printf("bad payload (0-8 hex bytes)\nERROR\n");
        return 1;
    }

    if (can_manager_send(id, ext, rtr, data, (uint8_t)dlc) != ESP_OK)
    {
        cmdline_printf("send failed (bus down or TX queue full)\nERROR\n");
        return 1;
    }

    cmdline_printf("queued %s%lX dlc %u\nOK\n", ext ? "x" : "",
                   (unsigned long)id, (unsigned)dlc);
    return 0;
}

static int cmd_can(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-z") == 0)
    {
        can_manager_zero_stats();
        cmdline_printf("counters zeroed\nOK\n");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "send") == 0)
    {
        return cmd_can_send(argc, argv);
    }

    can_manager_status_t st;

    (void)can_manager_status(&st);
    cmdline_printf("CAN: %s%s\n",
                   st.running ? "UP" : (st.enabled ? "DOWN" : "disabled"),
                   st.silent ? " (silent)" : "");
    cmdline_printf("Baud: %lu kbit/s\n", (unsigned long)st.baud_kbps);

    if (st.running)
    {
        cmdline_printf("State: %s\n", bus_state_str(st.stats.bus_state));
    }

    cmdline_printf("Tx: %lu  Rx: %lu\n",
                   (unsigned long)st.stats.tx_count,
                   (unsigned long)st.stats.rx_count);
    cmdline_printf("Errors: tx %lu, rx %lu, arb lost %lu, bus %lu\n",
                   (unsigned long)st.stats.tx_errors,
                   (unsigned long)st.stats.rx_errors,
                   (unsigned long)st.stats.arb_lost,
                   (unsigned long)st.stats.bus_errors);
    cmdline_printf("Bus-off: %lu (recovered %lu)\n",
                   (unsigned long)st.stats.bus_off_count,
                   (unsigned long)st.stats.recovery_count);
    cmdline_printf("OK\n");
    return 0;
}

esp_err_t can_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "can",
        .help = "CAN bus status ('can -z' zeroes counters, "
                "'can send <id> [hex]' transmits)",
        .func = cmd_can,
    };

    return cmdline_manager_register(&CMD);
}
