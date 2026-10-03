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
 * @file j1939_cli.c
 * @brief The `j1939` console command (§6b: registered from the settings
 *        apply, behind the `cli` setting):
 *
 *          j1939       state, bus verdict, counters, sources; in active
 *                      mode the address claim and the transmit counters
 *          j1939 -s    the built-in values the bus carries right now
 *          j1939 -d    active trouble codes (DM1) per controller
 *          j1939 -p    every stored message, first 8 bytes
 *          j1939 -r <pgn> [da]  active mode: send a Request for a group
 *                      (hex), to one controller (hex) or to everyone
 *          j1939 -z    zero the counters, forget the stored messages
 */
#include <stdlib.h>
#include <string.h>

#include "cmdline_manager.h"

#include "j1939.h"
#include "j1939_private.h"

#define CLI_SOURCES_MAX 16
#define CLI_DTCS_MAX    8

static const char *lamp_str(uint8_t lamp)
{
    switch (lamp)
    {
    case J1939_LAMP_OFF:   return "off";
    case J1939_LAMP_ON:    return "ON";
    case J1939_LAMP_ERROR: return "error";
    default:               return "-";
    }
}

static void print_summary(void)
{
    j1939_status_t st;
    j1939_source_t src[CLI_SOURCES_MAX];
    char vin[J1939_VIN_LEN + 1];
    uint8_t vin_sa = 0;

    (void)j1939_status(&st);
    cmdline_printf("J1939: %s, bus %s, mode %s\n", j1939_state_name(st.state),
                   j1939_bus_name(st.bus), st.active ? "active" : "listen");

    if (st.active)
    {
        cmdline_printf("Address: %s", j1939_claim_state_name(st.claim));

        if (st.address != J1939_ADDR_NULL)
        {
            cmdline_printf(" %u", st.address);
        }

        cmdline_printf(" (preferred %u, bus %s), NAME ",
                       j1939_settings_address(),
                       st.tx_ready ? "open" : "not transmit-ready");

        for (int k = J1939_NAME_LEN - 1; k >= 0; k--)
        {
            cmdline_printf("%02X", st.name[k]);
        }

        cmdline_printf("\nClaims: %lu sent, %lu contests (%lu won, %lu lost, "
                       "%lu not answered again), %lu answered requests, %lu "
                       "cannot-claim\n",
                       (unsigned long)st.claim_stats.claims_sent,
                       (unsigned long)st.claim_stats.contests,
                       (unsigned long)st.claim_stats.won,
                       (unsigned long)st.claim_stats.lost,
                       (unsigned long)st.claim_stats.held,
                       (unsigned long)st.claim_stats.requests,
                       (unsigned long)st.claim_stats.cannot);
        cmdline_printf("Sent: %lu frames (%lu failed), %lu requests: %lu acked, "
                       "%lu nacked; %lu requests to us nacked\n",
                       (unsigned long)st.tx_frames, (unsigned long)st.tx_failed,
                       (unsigned long)st.requests, (unsigned long)st.acks,
                       (unsigned long)st.nacks, (unsigned long)st.nacks_sent);
        cmdline_printf("Transport to us: %lu connections, %lu clear-to-send, "
                       "%lu end-of-message, %lu aborts sent, %lu replies lost\n",
                       (unsigned long)st.tp_to_me, (unsigned long)st.tp_cts,
                       (unsigned long)st.tp_eoma,
                       (unsigned long)st.tp_aborts_out,
                       (unsigned long)st.tp_reply_lost);
    }

    if (j1939_vin(vin, &vin_sa))
    {
        cmdline_printf("VIN: %s (from %02X)\n", vin, vin_sa);
    }

    cmdline_printf("Frames: %lu (groups %lu, tp.cm %lu, tp.dt %lu, "
                   "diagnostics %lu, foreign %lu)\n",
                   (unsigned long)st.rx_frames, (unsigned long)st.rx_data,
                   (unsigned long)st.rx_tp_cm, (unsigned long)st.rx_tp_dt,
                   (unsigned long)st.rx_diag, (unsigned long)st.rx_foreign);
    cmdline_printf("Lost before us: %lu  Messages stored: %lu\n",
                   (unsigned long)st.queue_drops, (unsigned long)st.messages);
    cmdline_printf("Store: %u of %u, not kept %lu, evicted %lu, long "
                   "payloads evicted %lu\n",
                   (unsigned)st.entries, (unsigned)st.entries_cap,
                   (unsigned long)st.not_kept, (unsigned long)st.evicted,
                   (unsigned long)st.long_evicted);
    cmdline_printf("Transport: %u of %u open, started %lu, completed %lu, "
                   "out of sequence %lu, timed out %lu\n",
                   (unsigned)st.tp_open, (unsigned)st.tp_cap,
                   (unsigned long)st.tp_started,
                   (unsigned long)st.tp_completed,
                   (unsigned long)st.tp_seq_errors,
                   (unsigned long)st.tp_timeouts);
    cmdline_printf("           aborted %lu, replaced %lu, no session %lu, "
                   "orphan packets %lu, bad announces %lu\n",
                   (unsigned long)st.tp_aborted, (unsigned long)st.tp_replaced,
                   (unsigned long)st.tp_no_session,
                   (unsigned long)st.tp_orphan_dt, (unsigned long)st.tp_bad_cm);

    size_t n = j1939_sources(src, CLI_SOURCES_MAX);

    cmdline_printf("Sources: %u\n", (unsigned)st.sources);

    for (size_t i = 0; i < n; i++)
    {
        cmdline_printf("  %02X: %lu frames, last %lu ms ago", src[i].sa,
                       (unsigned long)src[i].frames,
                       (unsigned long)src[i].age_ms);

        if (src[i].named)
        {
            cmdline_printf(", NAME ");

            for (int k = 0; k < 8; k++)
            {
                cmdline_printf("%02X", src[i].name[k]);
            }
        }

        cmdline_printf("\n");
    }

    if (st.sources > n)
    {
        cmdline_printf("  (%u more)\n", (unsigned)(st.sources - n));
    }
}

static void print_values(void)
{
    size_t count = 0;
    const j1939_spn_t *table = j1939_spn_table(&count);

    for (size_t i = 0; i < count; i++)
    {
        j1939_msg_t info;
        j1939_raw_t raw = J1939_RAW_SHORT;
        double value = 0;

        if (j1939_spn_latest(&table[i], J1939_ADDR_ANY, &value, &raw, &info) !=
            ESP_OK)
        {
            continue; /* nobody sends its group */
        }

        if (raw == J1939_RAW_VALID)
        {
            cmdline_printf("  %-26s %12.3f %-5s", table[i].name, value,
                           table[i].unit);
        }
        else
        {
            cmdline_printf("  %-26s %12s %-5s", table[i].name,
                           j1939_raw_name(raw), "");
        }

        cmdline_printf("  SPN %lu, %04lX from %02X, %lu ms ago\n",
                       (unsigned long)table[i].spn,
                       (unsigned long)table[i].pgn, info.sa,
                       (unsigned long)info.age_ms);
    }
}

static void print_dm1(void)
{
    j1939_source_t src[CLI_SOURCES_MAX];
    size_t n = j1939_sources(src, CLI_SOURCES_MAX);
    bool any = false;

    for (size_t i = 0; i < n; i++)
    {
        j1939_lamps_t lamps;
        j1939_dtc_t dtc[CLI_DTCS_MAX];
        j1939_msg_t info;
        size_t codes = 0;

        if (j1939_dm1(src[i].sa, &lamps, dtc, CLI_DTCS_MAX, &codes, &info) !=
            ESP_OK)
        {
            continue;
        }

        any = true;
        cmdline_printf("  %02X: %u code(s), MIL %s, red stop %s, amber %s, "
                       "protect %s, %lu ms ago\n",
                       src[i].sa, (unsigned)codes, lamp_str(lamps.mil),
                       lamp_str(lamps.rsl), lamp_str(lamps.awl),
                       lamp_str(lamps.pl), (unsigned long)info.age_ms);

        for (size_t k = 0; k < codes && k < CLI_DTCS_MAX; k++)
        {
            char text[J1939_DTC_TEXT_LEN];

            j1939_dtc_text(dtc[k].spn, dtc[k].fmi, text, sizeof(text));
            cmdline_printf("      %-13s seen %u time(s)%s\n", text,
                           (unsigned)dtc[k].oc,
                           dtc[k].cm ? ", old SPN layout flag" : "");
        }
    }

    if (!any)
    {
        cmdline_printf("  no controller sends DM1\n");
    }
}

static void print_messages(void)
{
    size_t cursor = 0;
    j1939_msg_t info;
    uint8_t data[8];

    while (j1939_msg_next(&cursor, &info, data, sizeof(data)))
    {
        cmdline_printf("  %05lX %02X>%02X len %4u n %-8lu every %5lu ms, "
                       "%6lu ms ago ",
                       (unsigned long)info.pgn, info.sa, info.da,
                       (unsigned)info.len, (unsigned long)info.count,
                       (unsigned long)info.period_ms,
                       (unsigned long)info.age_ms);

        for (unsigned k = 0; k < info.len && k < sizeof(data); k++)
        {
            cmdline_printf("%02X", data[k]);
        }

        cmdline_printf("%s\n", (info.len > sizeof(data)) ? ".." : "");
    }
}

static int cmd_j1939(int argc, char **argv)
{
    const char *opt = (argc > 1) ? argv[1] : "";

    if (!j1939_settings_is_configured())
    {
        cmdline_printf("j1939: not configured\nERROR\n");
        return 1;
    }

    if (strcmp(opt, "-z") == 0)
    {
        j1939_reset();
        cmdline_printf("counters zeroed, stored messages forgotten\nOK\n");
        return 0;
    }

    if (strcmp(opt, "-r") == 0)
    {
        if (argc < 3)
        {
            cmdline_printf("usage: j1939 -r <pgn hex> [da hex]\nERROR\n");
            return 1;
        }

        uint32_t pgn = (uint32_t)strtoul(argv[2], NULL, 16) & 0x3FFFFu;
        uint8_t da = (argc > 3) ? (uint8_t)strtoul(argv[3], NULL, 16)
                                : J1939_ADDR_GLOBAL;
        esp_err_t err = j1939_request(pgn, da);

        if (err == ESP_ERR_INVALID_STATE)
        {
            cmdline_printf("no address on the bus (listen mode, claim pending "
                           "or lost, or the bus is listen-only)\nERROR\n");
            return 1;
        }

        if (err != ESP_OK)
        {
            cmdline_printf("the bus did not take the frame\nERROR\n");
            return 1;
        }

        cmdline_printf("request for %05lX sent to %02X from %u\nOK\n",
                       (unsigned long)pgn, da, j1939_address());
        return 0;
    }

    if (strcmp(opt, "-s") == 0)
    {
        print_values();
    }
    else if (strcmp(opt, "-d") == 0)
    {
        print_dm1();
    }
    else if (strcmp(opt, "-p") == 0)
    {
        print_messages();
    }
    else if (opt[0] != '\0')
    {
        cmdline_printf("usage: j1939 [-s | -d | -p | -r <pgn> [da] | -z]\n"
                       "ERROR\n");
        return 1;
    }
    else
    {
        print_summary();
    }

    cmdline_printf("OK\n");
    return 0;
}

esp_err_t j1939_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "j1939",
        .help = "J1939: status ('-s' values, '-d' trouble codes, '-p' stored "
                "messages, '-r <pgn> [da]' request a group (active mode), "
                "'-z' zero and forget)",
        .func = cmd_j1939,
    };

    return cmdline_manager_register(&CMD);
}
