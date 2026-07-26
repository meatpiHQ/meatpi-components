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
 * @file external_storage_cli.c
 * @brief The component's CLI command (`sdcard`) — registered into
 *        cmdline_manager by external_storage_register_cli() (main wires
 *        it in CLI compositions only). Legacy option interface
 *        preserved (-i/--info, -t/--test); bare = presence/mount.
 *        FILESYSTEM usage lives in the `fs` command (filesystem owns
 *        that).
 */
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"

#include "cmdline_manager.h"

#include "external_storage.h"
#include "external_storage_private.h"

#define ES_TEST_FILE "/sd/.wican_selftest"

static struct
{
    struct arg_lit *info;
    struct arg_lit *test;
    struct arg_end *end;
} s_args;

static int sdcard_info(void)
{
    es_card_details_t card;

    if (!es_card_details(&card))
    {
        cmdline_printf("Error: Failed to read SD card info\n");
        return 1;
    }

    cmdline_printf("SD Card Info:\n");
    cmdline_printf("Name: %s\n", card.name);
    cmdline_printf("Type: %s\n", card.type);
    cmdline_printf("Capacity: %.2f GB\n",
                   (float)card.capacity_bytes / (1024.0f * 1024 * 1024));
    cmdline_printf("Sector Size: %d bytes\n", card.sector_size);
    cmdline_printf("Speed: %lu KHz\n", (unsigned long)card.speed_khz);
    cmdline_printf("OK\n");
    return 0;
}

/** Write/read-back/delete a marker file on the card (legacy -t). The
 *  card is SDMMC — no internal-flash cache involvement (§2-safe). */
static int sdcard_test(void)
{
    static const char PATTERN[] =
        "WiCAN SD self-test 0123456789 abcdefghijklmnopqrstuvwxyz";
    char readback[sizeof(PATTERN)];
    FILE *f = fopen(ES_TEST_FILE, "w");
    size_t n = 0;

    if (f != NULL)
    {
        n = fwrite(PATTERN, 1, sizeof(PATTERN), f);
        fclose(f);
    }

    if (f == NULL || n != sizeof(PATTERN))
    {
        cmdline_printf("Error: SD card test failed\n");
        return 1;
    }

    f = fopen(ES_TEST_FILE, "r");
    n = 0;

    if (f != NULL)
    {
        n = fread(readback, 1, sizeof(readback), f);
        fclose(f);
    }

    remove(ES_TEST_FILE);

    if (n != sizeof(PATTERN) ||
        memcmp(readback, PATTERN, sizeof(PATTERN)) != 0)
    {
        cmdline_printf("Error: SD card test failed\n");
        return 1;
    }

    cmdline_printf("SD card test passed successfully\n");
    cmdline_printf("OK\n");
    return 0;
}

static int cmd_sdcard(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);

    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.info->count > 0)
    {
        return sdcard_info();
    }

    if (s_args.test->count > 0)
    {
        if (!external_storage_is_mounted())
        {
            cmdline_printf("Error: SD card not mounted\n");
            return 1;
        }

        return sdcard_test();
    }

    cmdline_printf("Present: %s\n",
                   external_storage_is_present() ? "yes" : "no");
    cmdline_printf("Mounted: %s\n",
                   external_storage_is_mounted() ? "yes" : "no");
    cmdline_printf("OK\n");
    return 0;
}

esp_err_t external_storage_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "sdcard",
        .help = "SD card control and status",
        .hint = "Options: -i/--info, -t/--test",
        .func = cmd_sdcard,
        .argtable = &s_args,
    };

    s_args.info = arg_lit0("i", "info", "Get SD card information");
    s_args.test = arg_lit0("t", "test", "Test SD card read/write");
    s_args.end = arg_end(3);
    return cmdline_manager_register(&CMD);
}
