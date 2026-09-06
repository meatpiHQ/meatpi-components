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
 * @file autopid_settings.c
 * @brief settings_manager descriptor for autopid: field-table schema
 *        (source of truth for shape/ranges/defaults), on_apply (stores the
 *        boot-applied knobs here; pushes the runtime-owned ones into the
 *        core/runner/dtc modules via their hooks), on_migrate. Runtime
 *        state (scheduler, poller, config tables) stays in autopid.c.
 */
#include <stdio.h>
#include <string.h>

#include "dev_status_manager.h"
#include "settings_manager.h"

#include "autopid.h" /* autopid_register_cli (settings-gated §6b) */
#include "autopid_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),  /* off until the user configures it */
    SETTINGS_BOOL("std_enabled", true),
    SETTINGS_BOOL("custom_enabled", true),
    SETTINGS_BOOL("specific_enabled", true),
    SETTINGS_STR("std_init", AP_INIT_LEN - 1, ""),
    SETTINGS_STR("custom_init", AP_INIT_LEN - 1, ""),
    SETTINGS_STR("specific_init", AP_INIT_LEN - 1, ""),
    /* ISO 15765-4 CAN only (the MIC is a CAN chip): 6 = 11-bit 500k,
     * 7 = 29-bit 500k, 8 = 11-bit 250k, 9 = 29-bit 250k, 0 = chip auto.
     * An enum (meatpi 2026-09-06) so every client offers a list. */
    SETTINGS_STR_ENUM("std_protocol", "0,6,7,8,9", "6"),
    /* UI display label only: the name of the selected vehicle profile.
     * Persisted + returned by GET so the app can show which profile is
     * configured; intentionally NOT read in on_apply — the actual PID
     * tables come from /data/autopid/config.json (see autopid_config.c),
     * PUT via /api/autopid/config, not derived from this name. */
    SETTINGS_STR("vehicle", 63, ""),
    SETTINGS_INT("pause_below_mv", 0, 14500, 0), /* 0 = never pause      */
    /* legacy disable_pid_requests parity: with pause_below_mv unset,
       pause requests below the SLEEP voltage (engine-off signal) so
       polling can never hold the ECU awake on a parked car */
    SETTINGS_BOOL("pause_follow_sleep", true),
    SETTINGS_STR_ENUM("pause_mode", "all,requests_only", "requests_only"),
    SETTINGS_INT("min_event_interval_ms", 10, 600000, 1000),
    SETTINGS_BOOL("cli", true),
    /* DTC scan/report/clear (v3, TASK_dtc.md §6) — BOTH gates default
     * false (meatpi 2026-07-08): no bus activity until the user opts in,
     * and mode 04 needs its own second opt-in. */
    SETTINGS_BOOL("dtc_enabled", false),
    SETTINGS_BOOL("dtc_allow_clear", false),
    SETTINGS_INT("dtc_scan_period_min", 0, 10080, 0), /* 0 = on-demand */
    SETTINGS_BOOL("dtc_pending", true),      /* include mode 07         */
    SETTINGS_BOOL("dtc_permanent", false),   /* include mode 0A         */
    /* freeze frame (v5, TASK_dtc §14): capture mode-02 frame 0 during
     * an OBD scan when stored codes exist — default ON like
     * dtc_pending (extra requests only inside an already-opted-in scan
     * that actually found codes; the dtc_enabled gate stays the bus
     * opt-in). */
    SETTINGS_BOOL("dtc_freeze", true),
    SETTINGS_STR("dtc_init", AP_INIT_LEN - 1, ""),
    SETTINGS_STR("dtc_rxheader", AP_HDR_LEN - 1, ""),
    /* UDS DTC (v4, TASK_dtc §12 — meatpi 2026-07-22): obd = mode
     * 03/07/0A only (default, zero behavior change), uds = ISO 14229
     * 19/14 on the address pair below, auto = OBD first then UDS when
     * no ECU answers. */
    SETTINGS_STR_ENUM("dtc_protocol", "obd,uds,auto", "obd"),
    SETTINGS_STR("dtc_uds_txid", 8, "7E0"),
    SETTINGS_STR("dtc_uds_rxid", 8, "7E8"),
    SETTINGS_BOOL("dtc_uds_ext", false),
    SETTINGS_INT("dtc_uds_mask", 0, 255, 8), /* confirmedDTC           */
};

/* settings knobs (boot-applied) */
static char  s_std_protocol[8];
static uint32_t s_min_event_interval_ms = 1000;
static int   s_pause_below_mv;
static bool  s_pause_follow_sleep;
static bool  s_pause_all;                 /* pause_mode == "all"        */
static bool  s_configured;

bool ap_settings_is_configured(void)
{
    return s_configured;
}

int ap_settings_pause_below_mv(void)
{
    return s_pause_below_mv;
}

bool ap_settings_pause_follow_sleep(void)
{
    return s_pause_follow_sleep;
}

const char *ap_core_std_protocol(void)
{
    return s_std_protocol;
}

uint32_t ap_core_min_event_interval_ms(void)
{
    return s_min_event_interval_ms;
}

static void copy_setting(char *dst, size_t cap, const cJSON *settings,
                         const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    dst[0] = '\0';

    if (cJSON_IsString(v) && v->valuestring != NULL)
    {
        snprintf(dst, cap, "%s", v->valuestring);
    }
}

static bool setting_bool(const cJSON *settings, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *v;
    bool enabled = setting_bool(settings, "enabled", false);

    ap_core_set_enabled(enabled);
    if (enabled)
    {
        dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_ENABLED);
    }
    else
    {
        dev_status_manager_clear(DEV_STATUS_BIT_AUTOPID_ENABLED);
        dev_status_manager_set(DEV_STATUS_BIT_AUTOPID_IDLE);
    }
    ap_core_set_type_enabled(AP_PID_STD,
                             setting_bool(settings, "std_enabled", true));
    ap_core_set_type_enabled(AP_PID_CUSTOM,
                             setting_bool(settings, "custom_enabled",
                                          true));
    ap_core_set_type_enabled(AP_PID_SPECIFIC,
                             setting_bool(settings, "specific_enabled",
                                          true));

    char init[AP_INIT_LEN];

    copy_setting(init, sizeof(init), settings, "std_init");
    ap_runner_set_type_init(AP_PID_STD, init);
    copy_setting(init, sizeof(init), settings, "custom_init");
    ap_runner_set_type_init(AP_PID_CUSTOM, init);
    copy_setting(init, sizeof(init), settings, "specific_init");
    ap_runner_set_type_init(AP_PID_SPECIFIC, init);
    copy_setting(s_std_protocol, sizeof(s_std_protocol), settings,
                 "std_protocol");

    v = cJSON_GetObjectItemCaseSensitive(settings, "pause_below_mv");
    s_pause_below_mv = cJSON_IsNumber(v) ? v->valueint : 0;
    s_pause_follow_sleep = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "pause_follow_sleep"));

    v = cJSON_GetObjectItemCaseSensitive(settings, "min_event_interval_ms");
    s_min_event_interval_ms = cJSON_IsNumber(v) ? (uint32_t)v->valueint
                                                : 1000;

    char mode[20];

    copy_setting(mode, sizeof(mode), settings, "pause_mode");
    s_pause_all = (strcmp(mode, "all") == 0);

    ap_dtc_apply_settings(settings); /* dtc_* knobs (TASK_dtc.md §6) */

    /* CLI ownership: settings-gated self-registration (Standard §6b) */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && autopid_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

static esp_err_t ap_settings_migrate(uint32_t from_version, cJSON *settings)
{
    /* v1->v2 +backend, v2->v3 +dtc_*, v3->v4 +dtc_protocol/uds addr/mask,
       v4->v5 +dtc_freeze: defaults fill in. v5->v6 (2026-09-06): the
       `backend` field is gone — drop a stored one so it stops riding
       along in every read-modify-write */
    if (from_version < 6 && settings != NULL)
    {
        cJSON_DeleteItemFromObjectCaseSensitive(settings, "backend");
    }

    /* v6->v7 (2026-09-06): std_protocol became an enum (0,6,7,8,9). The
       old free text was read by its FIRST character (6..9, else auto) —
       keep exactly that meaning for whatever a device has stored */
    /* v7->v8 (2026-09-06): pause_below_mv capped at 14.5 V (a 12 V
       battery) — 0 keeps meaning "no fixed threshold" */
    if (from_version < 8 && settings != NULL)
    {
        cJSON *pv = cJSON_GetObjectItemCaseSensitive(settings, "pause_below_mv");

        if (cJSON_IsNumber(pv) && pv->valueint > 14500)
        {
            cJSON_SetNumberValue(pv, 14500);
        }
    }

    if (from_version < 7 && settings != NULL)
    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings,
                                                          "std_protocol");
        char c = (cJSON_IsString(v) && v->valuestring != NULL)
                     ? v->valuestring[0] : ' ';
        const char *mapped = (c >= '6' && c <= '9') ? (char[]){c, ' '} : "0";

        if (!cJSON_IsString(v) || v->valuestring == NULL ||
            strcmp(v->valuestring, mapped) != 0)
        {
            cJSON_ReplaceItemInObjectCaseSensitive(settings, "std_protocol",
                                                   cJSON_CreateString(mapped));
        }
    }

    return ESP_OK;
}

esp_err_t ap_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "autopid",
        .version     = 8, /* v8: pause_below_mv <= 14500 (2026-09-06);
                             v7: std_protocol enum (2026-09-06);
                             v6: -backend (2026-09-06);
                             v5: +dtc_freeze (TASK_dtc §14);
                             v4: +dtc_protocol/dtc_uds_* (TASK_dtc §12);
                             v3: +dtc_* (fill-missing defaults, both
                             gates false); v2: +backend */
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_migrate  = ap_settings_migrate,
    };

    return settings_manager_register(&DESC);
}
