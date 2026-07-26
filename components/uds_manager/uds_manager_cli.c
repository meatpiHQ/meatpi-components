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
 * @file uds_manager_cli.c
 * @brief The `uds` console command — the terminal's serial/BLE/WS face.
 *        `uds -t 7E0 -r 7E8 22 F1 90`  (hex request after the flags)
 */
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"

#include "cmdline_manager.h"

#include "uds_manager.h"
#include "uds_manager_private.h"
#include "uds_proto.h"

static struct
{
    struct arg_str *tx;
    struct arg_str *rx;
    struct arg_lit *ext;
    struct arg_str *data;
    struct arg_end *end;
} s_args;

static int cmd_uds(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&s_args);

    if (nerr != 0)
    {
        arg_print_errors(stdout, s_args.end, "uds");
        cmdline_printf("usage: uds -t <txid> -r <rxid> [-e] <hex request>\n");
        return 1;
    }

    if (s_args.tx->count == 0 || s_args.rx->count == 0 ||
        s_args.data->count == 0)
    {
        cmdline_printf("usage: uds -t 7E0 -r 7E8 [-e] 22 F1 90\n");
        return 1;
    }

    uds_addr_t addr =
    {
        .tx_id  = (uint32_t)strtoul(s_args.tx->sval[0], NULL, 16),
        .rx_id  = (uint32_t)strtoul(s_args.rx->sval[0], NULL, 16),
        .ext_id = s_args.ext->count > 0,
    };

    /* join the data args back into one hex string */
    char hexin[192];
    size_t o = 0;

    for (int i = 0; i < s_args.data->count && o < sizeof(hexin) - 1; i++)
    {
        int n = snprintf(hexin + o, sizeof(hexin) - o, "%s",
                         s_args.data->sval[i]);
        o += (n > 0) ? (size_t)n : 0;
    }

    uint8_t reqb[64];
    size_t reqn = 0;

    if (!uds_hex_to_bytes(hexin, reqb, sizeof(reqb), &reqn) || reqn == 0)
    {
        cmdline_printf("bad hex request\n");
        return 1;
    }

    static uint8_t respb[512];
    size_t respn = 0;
    uds_result_t res;

    esp_err_t err = uds_request(&addr, reqb, reqn, respb, sizeof(respb),
                                &respn, NULL, &res);

    if (err != ESP_OK)
    {
        cmdline_printf("uds error: %s (backend %s)\n",
                       esp_err_to_name(err), res.backend ? res.backend : "?");
        return 1;
    }

    char hex[3 * 128 + 1];
    uds_bytes_to_hex(respb, respn < 128 ? respn : 128, hex, sizeof(hex));

    cmdline_printf("<- %s  [%s, %lu ms", hex, res.backend,
                   (unsigned long)res.elapsed_ms);
    if (res.pending_count)
    {
        cmdline_printf(", %u pending", res.pending_count);
    }
    cmdline_printf("]\n");

    if (res.negative)
    {
        cmdline_printf("NEGATIVE: NRC 0x%02X %s\n", res.nrc, res.nrc_name);
    }
    else
    {
        cmdline_printf("positive (SID 0x%02X)\nOK\n", res.sid);
    }

    return 0;
}

esp_err_t uds_manager_register_cli(void)
{
    s_args.tx = arg_str0("t", "tx", "<hhh>", "request CAN ID (hex)");
    s_args.rx = arg_str0("r", "rx", "<hhh>", "response CAN ID (hex)");
    s_args.ext = arg_lit0("e", "ext", "29-bit extended IDs");
    s_args.data = arg_strn(NULL, NULL, "<hex>", 0, 32, "request bytes");
    s_args.end = arg_end(4);

    static const esp_console_cmd_t CMD =
    {
        .command = "uds",
        .help = "UDS request: uds -t 7E0 -r 7E8 [-e] 22 F1 90",
        .func = cmd_uds,
        .argtable = &s_args,
    };

    return cmdline_manager_register(&CMD);
}
