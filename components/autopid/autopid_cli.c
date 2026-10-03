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
 * @file autopid_cli.c
 * @brief The `autopid` console command (replaces main_cli's pending stub).
 *        Registered from the component's settings apply (Standard §6b).
 */
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmdline_manager.h"

#include "autopid.h"
#include "autopid_private.h"

static struct
{
    struct arg_lit *list;
    struct arg_lit *dtc;
    struct arg_lit *dtc_scan;
    struct arg_end *end;
} s_args;

/** One category of the report's JSON view: "stored (2): P0420 P0171". */
static void print_codes(const cJSON *report, const char *key)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(report, key);
    const cJSON *c = NULL;

    cmdline_printf("%s (%d):", key, cJSON_GetArraySize(arr));

    cJSON_ArrayForEach(c, arr)
    {
        if (cJSON_IsString(c))
        {
            cmdline_printf(" %s", c->valuestring);
        }
    }

    cmdline_printf("\n");
}

/** Who asks for the lamp: "ECU 18DAF100: MIL on, 1 code". */
static void print_sources(const cJSON *report)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(report, "sources");
    const cJSON *s = NULL;

    cJSON_ArrayForEach(s, arr)
    {
        const cJSON *ecu = cJSON_GetObjectItemCaseSensitive(s, "ecu");
        const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(s, "count");

        cmdline_printf("ECU %s: MIL %s, %d code(s)\n",
                       cJSON_IsString(ecu) ? ecu->valuestring : "?",
                       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s,
                                                                    "mil"))
                           ? "on" : "off",
                       cJSON_IsNumber(cnt) ? cnt->valueint : 0);
    }
}

static int cmd_dtc(bool scan)
{
    if (scan)
    {
        esp_err_t err = ap_dtc_scan_start();

        if (err == ESP_ERR_NOT_ALLOWED)
        {
            cmdline_printf("dtc_enabled is off\n");
            return 1;
        }

        if (err != ESP_OK)
        {
            cmdline_printf("scan not started (busy?)\n");
            return 1;
        }

        /* worst case = 4 requests x 5 s (SEARCHING on a cold protocol) */
        int waited = 0;

        while (ap_dtc_busy() && waited < 25000)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited += 200;
        }
    }

    /* the JSON view (heap): the report itself is ~5 KB and stays where
       it is */
    cJSON *report = ap_dtc_report_json();

    if (report == NULL)
    {
        cmdline_printf("out of memory\n");
        return 1;
    }

    const cJSON *err = cJSON_GetObjectItemCaseSensitive(report, "error");
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(report, "protocol");
    const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(report, "mil_count");
    const char *error = cJSON_IsString(err) ? err->valuestring : "";

    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "valid")))
    {
        cmdline_printf("no valid scan yet%s%s\n",
                       (error[0] != '\0') ? " - " : "", error);
        cJSON_Delete(report);
        return scan ? 1 : 0;
    }

    cmdline_printf("MIL: %s (count %d), protocol %s\n",
                   cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report,
                                                                "mil"))
                       ? "ON" : "off",
                   cJSON_IsNumber(cnt) ? cnt->valueint : 0,
                   cJSON_IsString(proto) ? proto->valuestring : "?");
    print_sources(report);
    print_codes(report, "stored");
    print_codes(report, "pending");
    print_codes(report, "permanent");
    print_codes(report, "new");

    if (error[0] != '\0')
    {
        cmdline_printf("last error: %s\n", error);
    }

    cJSON_Delete(report);
    cmdline_printf("OK\n");
    return 0;
}

static int cmd_autopid(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&s_args);

    if (errors != 0)
    {
        cmdline_printf("Usage: autopid [-l] [-d] [--dtc-scan]\n");
        return 1;
    }

    if (s_args.dtc->count > 0 || s_args.dtc_scan->count > 0)
    {
        return cmd_dtc(s_args.dtc_scan->count > 0);
    }

    autopid_stats_t st;

    autopid_stats(&st);
    cmdline_printf("AutoPID: %s%s%s%s%s\n",
                   st.running ? "running" : "idle",
                   st.paused_voltage ? " (voltage pause)" : "",
                   st.paused_client ? " (yielding to an OBD app)" : "",
                   st.paused_diag ? " (paused for a diagnostic tool)" : "",
                   st.paused_bus ? " (bus guard: nothing is transmitted)"
                                 : "");
    cmdline_printf("Tables: %lu pids, %lu filters, %lu params, %lu groups\n",
                   (unsigned long)st.pids_loaded,
                   (unsigned long)st.filters_loaded,
                   (unsigned long)st.params_loaded,
                   (unsigned long)st.groups_loaded);
    cmdline_printf("Polls: %lu ok, %lu failed\n",
                   (unsigned long)st.polls_ok,
                   (unsigned long)st.polls_failed);

    if (st.passive_ok != 0 || st.passive_failed != 0)
    {
        /* J1939 rows, read from the listener's store (autopid_j1939.h) */
        uint32_t published = 0, requested = 0, refused = 0;

        ap_runner_j1939_stats(&published, NULL);
        ap_runner_j1939_tx_stats(&requested, &refused);
        cmdline_printf("J1939 rows: %lu looks found the group, %lu not, "
                       "%lu published, %lu requests sent, %lu refused\n",
                       (unsigned long)st.passive_ok,
                       (unsigned long)st.passive_failed,
                       (unsigned long)published, (unsigned long)requested,
                       (unsigned long)refused);
    }

    if (s_args.list->count > 0)
    {
        const ap_config_t *cfg = ap_core_config();
        int64_t now = esp_timer_get_time();

        for (uint16_t i = 0; i < cfg->n_params; i++)
        {
            double value;
            int64_t ts;

            if (ap_cache_get(i, &value, &ts))
            {
                cmdline_printf("%-24s %10.2f %-8s (%lld ms ago)\n",
                               cfg->params[i].name, value,
                               cfg->params[i].unit,
                               (long long)((now - ts) / 1000));
            }
            else
            {
                cmdline_printf("%-24s        --- %s\n",
                               cfg->params[i].name, cfg->params[i].unit);
            }
        }
    }

    cmdline_printf("OK\n");
    return 0;
}

esp_err_t autopid_register_cli(void)
{
    s_args.list = arg_lit0("l", "list", "list parameters + latest values");
    s_args.dtc = arg_lit0("d", "dtc", "show the last DTC report");
    s_args.dtc_scan = arg_lit0(NULL, "dtc-scan",
                               "run a DTC scan, then show the report");
    s_args.end = arg_end(3);

    static const esp_console_cmd_t CMD =
    {
        .command = "autopid",
        .help = "AutoPID status, values, and DTC report",
        .hint = "Usage: autopid [-l] [-d] [--dtc-scan]",
        .func = cmd_autopid,
        .argtable = &s_args,
    };

    return cmdline_manager_register(&CMD);
}
