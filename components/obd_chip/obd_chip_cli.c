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
 * @file obd_chip_cli.c
 * @brief The `obd_chip` console command (standard §6b, registered from the
 *        settings apply behind `cli`, 2026-10-28): the chip's state and
 *        firmware version, the packaged image and its policy, a forced
 *        update as a background job, and a bare-wire probe for a chip that
 *        will not come up. Issue #1023: a stuck MIC3624 could neither be
 *        diagnosed nor reflashed from the device; the web UI shows none of
 *        this on purpose (meatpi 2026-09-07: chip knobs stay API/CLI only).
 */
#include "obd_chip.h"

#include <stdlib.h>
#include <string.h>

#include "cmdline_manager.h"

#include "obd_chip_private.h"

#define CLI_PROBE_BYTES 32

static void print_status(void)
{
    obd_chip_stats_t st;
    obd_chip_fw_status_t fw;
    char ver[64];

    obd_chip_get_stats(&st);
    obd_chip_fw_status(&fw);

    cmdline_printf("chip:    ready=%d claim=%s baud=%d rx_buffered=%lu\n",
                   st.ready ? 1 : 0, st.claim, obd_chip_active_baud(),
                   (unsigned long)st.rx_buffered);

    if (fw.running)
    {
        cmdline_printf("version: - (update running)\n");
    }
    else if (st.ready &&
             obd_chip_get_version(ver, sizeof(ver), pdMS_TO_TICKS(3000)) ==
                 ESP_OK)
    {
        cmdline_printf("version: %s\n", ver);
    }
    else
    {
        cmdline_printf("version: - (%s)\n",
                       st.ready ? "no answer" : "chip not up; see: obd_chip probe");
    }

    cmdline_printf("builtin: %s (auto_update %s; current: %s)\n",
                   obd_chip_builtin_fw_version(),
                   obd_settings_config()->auto_update ? "on" : "off",
                   obd_fw_current_versions());

    if (fw.running)
    {
        cmdline_printf("update:  running %lu/%lu records (%s)\n",
                       (unsigned long)fw.lines, (unsigned long)fw.total,
                       fw.source);
    }
    else if (fw.done_once)
    {
        cmdline_printf("update:  idle; last %s: %s\n", fw.source,
                       esp_err_to_name(fw.last_err));
    }
    else
    {
        cmdline_printf("update:  idle\n");
    }
}

static int do_update(int argc, char **argv)
{
    bool force = false;
    const char *path = NULL;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--force") == 0)
        {
            force = true;
        }
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
        {
            path = argv[++i];
        }
        else
        {
            cmdline_printf("usage: obd_chip update [--force] [--file <path>]\n");
            return 1;
        }
    }

    esp_err_t err = obd_chip_firmware_update_start(path, force);

    if (err == ESP_ERR_INVALID_STATE)
    {
        cmdline_printf("an update is already running\n");
        return 1;
    }

    if (err != ESP_OK)
    {
        cmdline_printf("could not start the update (%s)\n",
                       esp_err_to_name(err));
        return 1;
    }

    cmdline_printf("update started from %s%s: about 90 s, the chip is busy "
                   "meanwhile; `obd_chip` shows the progress\n",
                   (path != NULL) ? path : "the packaged image",
                   force ? " (forced)" : "");
    return 0;
}

static void print_probe(int baud, const char *cmd, const uint8_t *rx,
                        size_t stored, size_t got, bool prompt)
{
    char text[CLI_PROBE_BYTES + 1];
    char hex[3 * CLI_PROBE_BYTES + 1];
    size_t n = 0;

    for (size_t i = 0; i < stored; i++)
    {
        text[i] = (rx[i] >= 32 && rx[i] < 127) ? (char)rx[i] : '.';
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%s%02X",
                              (i > 0) ? " " : "", rx[i]);
    }

    text[stored] = '\0';
    cmdline_printf("  %7d %-7s prompt=%d %3u B '%s'%s %s\n", baud, cmd,
                   prompt ? 1 : 0, (unsigned)got, text,
                   (got > stored) ? " ..." : "", hex);
}

/** `obd_chip probe scan`: ATI at every standard baud (Ali's question on
 *  issue #1023: a chip stuck at a baud nobody expects). Prints the bauds
 *  that returned anything at all and names the first one with a prompt. */
/* the standard bauds: the scan walks them, the detector names the nearest */
static const int BAUD_TABLE[] = { 9600, 19200, 38400, 57600, 115200, 230400,
                                  460800, 500000, 921600, 1000000, 1500000,
                                  2000000, 3000000 };
#define BAUD_TABLE_N (sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]))

static int do_probe_scan(void)
{
    int found = 0;

    cmdline_printf("baud scan with ATI (about %u s, the chip is busy):\n",
                   (unsigned)BAUD_TABLE_N);

    for (size_t b = 0; b < BAUD_TABLE_N; b++)
    {
        uint8_t rx[CLI_PROBE_BYTES];
        size_t got = 0;
        bool prompt = false;

        /* a lone CR first: a chip mid-line answers the next command clean */
        (void)obd_chip_probe("\r", BAUD_TABLE[b], 150, rx, sizeof(rx), &got,
                             &prompt);

        esp_err_t err = obd_chip_probe("ATI\r", BAUD_TABLE[b], 600, rx,
                                       sizeof(rx), &got, &prompt);

        if (err != ESP_OK)
        {
            cmdline_printf("  %7d ATI     %s\n", BAUD_TABLE[b],
                           esp_err_to_name(err));
            continue;
        }

        if (got > 0)
        {
            print_probe(BAUD_TABLE[b], "ATI", rx,
                        (got < sizeof(rx)) ? got : sizeof(rx), got, prompt);
        }

        if (prompt && found == 0)
        {
            found = BAUD_TABLE[b];
        }
    }

    if (found != 0)
    {
        cmdline_printf("prompt at %d baud\n", found);
    }
    else
    {
        cmdline_printf("no prompt at any baud (bytes above, if any, are "
                       "what arrived)\n");
    }

    return 0;
}

/** `obd_chip listen <seconds> [baud]`: send nothing, print what the chip
 *  sends on its own (issue #1023: a chip filling the RX ring with about
 *  two bytes every 5 s while the ESP is silent; at the right baud that is
 *  a banner, a reboot loop, a bootloader prompt, readable as text). */
static int listen_at(int baud, int seconds)
{
    static uint8_t rx[512]; /* a few seconds of a trickle, or one banner */
    size_t got = 0;
    bool prompt = false;

    cmdline_printf("listening %d s at %d baud, sending nothing:\n", seconds,
                   (baud > 0) ? baud : obd_uart_get_baud());

    esp_err_t err = obd_chip_probe("", baud, (uint32_t)seconds * 1000u, rx,
                                   sizeof(rx), &got, &prompt);

    if (err != ESP_OK)
    {
        cmdline_printf("  %s\n", esp_err_to_name(err));
        return 1;
    }

    if (got == 0)
    {
        cmdline_printf("  nothing received\n");
        return 0;
    }

    size_t stored = (got < sizeof(rx)) ? got : sizeof(rx);
    char text[CLI_PROBE_BYTES + 1];

    for (size_t off = 0; off < stored; off += CLI_PROBE_BYTES)
    {
        size_t n = ((stored - off) < CLI_PROBE_BYTES) ? (stored - off)
                                                     : CLI_PROBE_BYTES;
        char hex[3 * CLI_PROBE_BYTES + 1];
        size_t h = 0;

        for (size_t i = 0; i < n; i++)
        {
            uint8_t c = rx[off + i];

            text[i] = (c >= 32 && c < 127) ? (char)c : '.';
            h += (size_t)snprintf(hex + h, sizeof(hex) - h, "%s%02X",
                                  (i > 0) ? " " : "", c);
        }

        text[n] = '\0';
        cmdline_printf("  '%s' %s\n", text, hex);
    }

    cmdline_printf("  %u byte(s) in %d s%s%s\n", (unsigned)got, seconds,
                   prompt ? ", ended at a prompt" : "",
                   (got > stored) ? " (first 512 shown)" : "");
    return 0;
}

static int do_listen(int argc, char **argv)
{
    int seconds = (argc > 2) ? atoi(argv[2]) : 5;
    int baud = (argc > 3) ? atoi(argv[3]) : 0;

    if (seconds <= 0 || seconds > 60 || baud < 0)
    {
        cmdline_printf("usage: obd_chip listen <seconds 1..60> [baud]\n");
        return 1;
    }

    return listen_at(baud, seconds);
}

/** The table entry within 8 % of @p baud, else 0. */
static int nearest_standard_baud(int baud)
{
    for (size_t i = 0; baud > 0 && i < BAUD_TABLE_N; i++)
    {
        int d = baud - BAUD_TABLE[i];

        if (d < 0)
        {
            d = -d;
        }

        if ((int64_t)d * 100 <= (int64_t)BAUD_TABLE[i] * 8)
        {
            return BAUD_TABLE[i];
        }
    }

    return 0;
}

/** `obd_chip probe auto [seconds]`: the UART hardware measures the bit
 *  time of whatever the chip sends (Ali, issue #1023: a chip stuck at a
 *  baud nobody tries). A CR and ATI go out at the current baud first so a
 *  quiet chip has something to answer; a chip talking on its own (a reboot
 *  loop's banner) is measured as it comes. Then a listen at that baud. */
static int do_probe_auto(int argc, char **argv)
{
    int seconds = (argc > 3) ? atoi(argv[3]) : 10;
    obd_chip_autobaud_t ab;

    if (seconds <= 0 || seconds > 60)
    {
        cmdline_printf("usage: obd_chip probe auto [seconds 1..60]\n");
        return 1;
    }

    cmdline_printf("baud detection: CR + ATI at %d baud, then %d s of "
                   "measuring the RX edges:\n", obd_uart_get_baud(), seconds);

    esp_err_t err = obd_chip_autobaud("\rATI\r", (uint32_t)seconds * 1000u, &ab);

    if (err != ESP_OK)
    {
        cmdline_printf("  %s\n", esp_err_to_name(err));
        return 1;
    }

    if (ab.edges == 0)
    {
        cmdline_printf("  no edges: the chip sent nothing in %d s\n", seconds);
        return 0;
    }

    cmdline_printf("  %lu edges, ticks at %lu Hz: low %lu high %lu pos %lu "
                   "neg %lu\n", (unsigned long)ab.edges,
                   (unsigned long)ab.clk_hz, (unsigned long)ab.low,
                   (unsigned long)ab.high, (unsigned long)ab.pos,
                   (unsigned long)ab.neg);
    cmdline_printf("  bit time says %d baud (pulses), %d (positive edges), "
                   "%d (negative edges)\n", ab.baud_lh, ab.baud_pos,
                   ab.baud_neg);

    int near = nearest_standard_baud(ab.baud_lh);

    if (near == 0)
    {
        cmdline_printf("  no standard baud within 8 %% of %d\n", ab.baud_lh);
        return 0;
    }

    cmdline_printf("  nearest standard baud: %d\n", near);
    return listen_at(near, 6);
}

static int do_probe(int argc, char **argv)
{
    const int both[2] = { OBD_CHIP_BAUD, OBD_CHIP_FALLBACK_BAUD };
    const char *cmds[2] = { "ATI", "VTVERS" };
    int bauds[2];
    size_t n_bauds = 2;

    if (argc > 2 && strcmp(argv[2], "scan") == 0)
    {
        return do_probe_scan();
    }

    if (argc > 2 && strcmp(argv[2], "auto") == 0)
    {
        return do_probe_auto(argc, argv);
    }

    if (argc > 2)
    {
        bauds[0] = atoi(argv[2]);
        n_bauds = 1;

        if (bauds[0] <= 0)
        {
            cmdline_printf("usage: obd_chip probe [baud|scan|auto [seconds]]\n");
            return 1;
        }
    }
    else
    {
        bauds[0] = both[0];
        bauds[1] = both[1];
    }

    cmdline_printf("bare-wire probe (the chip is busy for a few seconds):\n");

    for (size_t b = 0; b < n_bauds; b++)
    {
        for (size_t c = 0; c < 2; c++)
        {
            uint8_t rx[CLI_PROBE_BYTES];
            size_t got = 0;
            bool prompt = false;
            char line[16];

            snprintf(line, sizeof(line), "%s\r", cmds[c]);

            esp_err_t err = obd_chip_probe(line, bauds[b], 1500, rx,
                                           sizeof(rx), &got, &prompt);

            if (err != ESP_OK)
            {
                cmdline_printf("  %7d %-7s %s\n", bauds[b], cmds[c],
                               esp_err_to_name(err));
                continue;
            }

            print_probe(bauds[b], cmds[c], rx,
                        (got < sizeof(rx)) ? got : sizeof(rx), got, prompt);
        }
    }

    return 0;
}

static int cmd_obd_chip(int argc, char **argv)
{
    if (argc < 2)
    {
        print_status();
        return 0;
    }

    if (strcmp(argv[1], "update") == 0)
    {
        return do_update(argc, argv);
    }

    if (strcmp(argv[1], "probe") == 0)
    {
        return do_probe(argc, argv);
    }

    if (strcmp(argv[1], "listen") == 0)
    {
        return do_listen(argc, argv);
    }

    cmdline_printf("usage: obd_chip | obd_chip update [--force] [--file "
                   "<path>] | obd_chip probe [baud|scan|auto [seconds]] | "
                   "obd_chip listen <seconds> [baud]\n");
    return 1;
}

esp_err_t obd_chip_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "obd_chip",
        .help = "OBD chip (MIC3624): state, firmware version, update, probe, "
                "listen",
        .hint = "Usage: obd_chip | obd_chip update [--force] [--file <path>] "
                "| obd_chip probe [baud|scan|auto [seconds]] | obd_chip "
                "listen <seconds> [baud]",
        .func = cmd_obd_chip,
    };

    return cmdline_manager_register(&CMD);
}
