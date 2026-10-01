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
 * @file autopid_std_scan.c
 * @brief The async standard-PID support scan ("scan once, store",
 *        TASK_autopid.md §7) grown into the vehicle DETECTION job
 *        (TASK_quick_setup.md, second pass). Split out of autopid_std.c
 *        2026-10-01.
 *
 * POST /api/autopid/vehicles/detect (alias: POST /api/autopid/std_scan)
 * spawns a short-lived task (INTERNAL stack: it writes /data at the end,
 * §2) that pauses the poller and runs three phases, reported as `phase`
 * in the status JSON:
 *   protocol  only with `std_protocol` = "0": ATTP0 prelude, 0100 (the
 *             chip prints SEARCHING... so the timeout is long), ATDPN,
 *             then the detected protocol's prelude for the rest;
 *   vin       0902; on failure 22F190 on the engine ECU's physical
 *             address (header restored), then the responder set from a
 *             headers-on 0100 (the fingerprint);
 *   pids      the 0100/0120/../01A0 support bitmap walk (multi-ECU
 *             OR-merged) mapped onto the SAE table.
 * The result goes to the vehicle store (autopid_vehicle_detected: a
 * known car comes back with its own tables, an unknown car becomes a new
 * entry polled on the standard rows found here), then to
 * /data/autopid/std_scan.json (atomic). The poller starts this job by
 * itself once per boot when first contact meets an unknown car; the UI
 * owns the button otherwise.
 */
#include "autopid_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "filesystem.h"
#include "obd_chip.h"

static const char *TAG = "autopid";

#define AP_STD_SCAN_PATH    "/data/autopid/std_scan.json"
#define AP_SCAN_REQ_TIMEOUT pdMS_TO_TICKS(10000) /* SEARCHING can be slow  */
#define AP_SCAN_VIN_TIMEOUT pdMS_TO_TICKS(6000)  /* multi-frame reply      */
#define AP_SCAN_AT_TIMEOUT  pdMS_TO_TICKS(2000)

typedef enum
{
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_DONE,
    SCAN_FAILED,
} scan_state_t;

typedef enum
{
    PHASE_IDLE = 0,
    PHASE_PROTOCOL,
    PHASE_VIN,
    PHASE_PIDS,
} scan_phase_t;

static volatile scan_state_t s_scan_state;
static volatile scan_phase_t s_scan_phase;
static uint16_t s_scan_found;
static char     s_scan_err[64];
static int64_t  s_scan_ts;      /* epoch seconds of last completed scan */

static StaticTask_t s_scan_tcb;                    /* internal object    */
static StackType_t  s_scan_stack[6144];            /* INTERNAL: fs write */

static char s_resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR;  /* scan task only     */

const char *autopid_std_scan_path(void)
{
    return AP_STD_SCAN_PATH;
}

const char *ap_std_prelude(void)
{
    /* The chip baseline for the scan AND the poller (its boot prelude and
       the restore after an external ELM app / diagnostic tool had the
       chip - bench 2026-09-08: Car Scanner's ATS0 made every resumed poll
       fail to parse until the next reboot). A pinned `std_protocol`
       (6..9) wins; "0" pins the current car's protocol from the vehicle
       store, or ATTP0 (chip search) when there is none or it went silent
       this boot (ap_runner_proto_fallback). */
    return ap_veh_prelude_for(ap_veh_effective_protocol(
        ap_core_std_protocol(), autopid_vehicle_protocol(),
        ap_runner_proto_fallback()));
}

/* ---- chip helpers (scan task context) ----------------------------------------- */

static esp_err_t scan_request(const char *cmd, TickType_t timeout)
{
    s_resp[0] = '\0';
    return obd_chip_request(cmd, s_resp, sizeof(s_resp), timeout);
}

/** Send a ';'-separated prelude one command at a time. */
static void scan_send_prelude(const char *prelude)
{
    const char *p = prelude;

    while (*p != '\0')
    {
        const char *sep = strchr(p, ';');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char one[24];

        if (len > 0 && len < sizeof(one))
        {
            memcpy(one, p, len);
            one[len] = '\0';
            (void)scan_request(one, AP_SCAN_AT_TIMEOUT);
        }

        p += len + ((sep != NULL) ? 1 : 0);
    }
}

/* ---- phase: protocol ------------------------------------------------------------- */

/** ATTP0 prelude, 0100 (lets the chip search), ATDPN.
 *  @return the protocol char, or '\0' when the car did not answer. */
static char scan_detect_protocol(void)
{
    scan_send_prelude(ap_veh_prelude_for('0'));

    uint32_t bitmap = 0;

    if (scan_request("0100", AP_SCAN_REQ_TIMEOUT) != ESP_OK ||
        !ap_std_scan_parse(s_resp, 0x00, &bitmap))
    {
        return '\0';
    }

    char dpn[AP_VEH_PROTO_LEN];

    if (scan_request("ATDPN", AP_SCAN_AT_TIMEOUT) != ESP_OK ||
        !ap_veh_parse_dpn(s_resp, dpn))
    {
        ESP_LOGW(TAG, "std scan: ATDPN reply unreadable ('%s')", s_resp);
        return '\0';
    }

    return dpn[0];
}

/* ---- phase: vin ---------------------------------------------------------------------- */

static void scan_read_vin(char proto, char vin[AP_VIN_LEN])
{
    vin[0] = '\0';

    if (scan_request("0902", AP_SCAN_VIN_TIMEOUT) == ESP_OK &&
        ap_veh_parse_vin_0902(s_resp, vin))
    {
        return;
    }

    /* UDS fallback: DID F190 on the engine ECU's physical address, CAN
       protocols only; the functional header is put back either way */
    if (proto < '6' || proto > '9')
    {
        return;
    }

    bool ext = ap_veh_proto_is_29bit(proto);

    (void)scan_request(ext ? "ATSH18DA10F1" : "ATSH7E0", AP_SCAN_AT_TIMEOUT);

    if (scan_request("22F190", AP_SCAN_VIN_TIMEOUT) == ESP_OK)
    {
        (void)ap_veh_parse_vin_22f190(s_resp, vin);
    }

    (void)scan_request(ext ? "ATSH18DB33F1" : "ATSH7DF", AP_SCAN_AT_TIMEOUT);
}

/** The responder set (behind the fingerprint): headers on for ONE 0100,
 *  then off again. */
static void scan_responders(ap_veh_seen_t *seen)
{
    seen->n_ecus = 0;

    if (scan_request("ATH1", AP_SCAN_AT_TIMEOUT) != ESP_OK)
    {
        return;
    }

    if (scan_request("0100", AP_SCAN_REQ_TIMEOUT) == ESP_OK)
    {
        seen->n_ecus = (uint8_t)ap_veh_ecus_from_0100(s_resp, seen->ecus,
                                                     AP_VEH_ECUS_MAX);
    }

    (void)scan_request("ATH0", AP_SCAN_AT_TIMEOUT);
}

/** The scanned rows as a config.json document (a new car's tables:
 *  standard PIDs only, the `default` group). Caller frees. */
static char *std_rows_config(const cJSON *supported)
{
    cJSON *root = cJSON_CreateObject();
    char *body = NULL;

    if (root == NULL)
    {
        return NULL;
    }

    cJSON_AddArrayToObject(root, "groups");

    cJSON *pids = cJSON_AddArrayToObject(root, "pids");
    const cJSON *e = NULL;

    cJSON_ArrayForEach(e, supported)
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(e, "name");
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(e, "cmd");
        const cJSON *prm = cJSON_GetObjectItemCaseSensitive(e, "parameters");
        cJSON *pid = cJSON_CreateObject();

        if (pids == NULL || pid == NULL || !cJSON_IsString(name) ||
            !cJSON_IsString(cmd))
        {
            cJSON_Delete(pid);
            continue;
        }

        cJSON_AddStringToObject(pid, "name", name->valuestring);
        cJSON_AddStringToObject(pid, "type", "std");
        cJSON_AddStringToObject(pid, "cmd", cmd->valuestring);
        cJSON_AddStringToObject(pid, "group", "default");
        cJSON_AddItemToObject(pid, "parameters",
                              cJSON_IsArray(prm) ? cJSON_Duplicate(prm, true)
                                                 : cJSON_CreateArray());
        cJSON_AddItemToArray(pids, pid);
    }

    cJSON_AddArrayToObject(root, "filters");
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ---- phase: pids ------------------------------------------------------------------- */

/** The bitmap walk. @return true when at least one range answered. */
static bool scan_pids(cJSON *supported, uint16_t *found)
{
    bool any_response = false;

    for (int range = 0; range < 6; range++)
    {
        uint8_t base = (uint8_t)(range * 0x20);
        char cmd[8];

        snprintf(cmd, sizeof(cmd), "01%02X", base);

        if (scan_request(cmd, AP_SCAN_REQ_TIMEOUT) != ESP_OK)
        {
            break;
        }

        uint32_t bitmap = 0;

        if (!ap_std_scan_parse(s_resp, base, &bitmap))
        {
            break;      /* NO DATA / noise: range unsupported */
        }

        any_response = true;

        for (int bit = 0; bit < 31; bit++)  /* bit 31 = next-range flag */
        {
            if (bitmap & (1u << (31 - bit)))
            {
                uint8_t pid = (uint8_t)(base + bit + 1);

                if (ap_std_entry_to_json(supported, pid))
                {
                    (*found)++;
                }
            }
        }

        if ((bitmap & 1u) == 0)
        {
            break;      /* next range not supported */
        }
    }

    return any_response;
}

/* ---- the job ------------------------------------------------------------------------ */

/** Hand the result to the vehicle store and stamp the result document
 *  with the car it landed on (`key`, `known`, `name`). */
static void scan_store(cJSON *root, const cJSON *supported,
                       ap_veh_seen_t *seen, uint16_t found)
{
    static ap_veh_entry_t entry EXT_RAM_BSS_ATTR; /* scan task only     */
    bool known = false;
    char *tables = std_rows_config(supported);
    esp_err_t err = ESP_ERR_NO_MEM;

    seen->std_supported = found;

    if (tables != NULL)
    {
        err = autopid_vehicle_detected(seen, tables, strlen(tables), &entry,
                                       &known);
        free(tables);
    }

    if (err == ESP_OK)
    {
        cJSON_AddStringToObject(root, "key", entry.key);
        cJSON_AddBoolToObject(root, "known", known);
        cJSON_AddStringToObject(root, "name", entry.name);
    }
    else
    {
        /* a car without VIN or responders cannot be stored: the table
           still is (the user can merge it by hand) */
        ESP_LOGW(TAG, "detection: not stored (%s)", esp_err_to_name(err));
        cJSON_AddStringToObject(root, "key", "");
        cJSON_AddBoolToObject(root, "known", false);
        cJSON_AddStringToObject(root, "name", "");
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    ap_core_scan_pause(true);

    static ap_veh_seen_t seen EXT_RAM_BSS_ATTR;      /* scan task only  */
    const char *setting = ap_core_std_protocol();
    bool pinned = (setting[0] >= '6' && setting[0] <= '9');
    char proto = pinned ? setting[0] : '\0';
    char fp[AP_FP_LEN] = "";
    bool no_ecu = false;

    memset(&seen, 0, sizeof(seen));

    if (!pinned)
    {
        s_scan_phase = PHASE_PROTOCOL;
        proto = scan_detect_protocol();
        no_ecu = (proto == '\0');
    }

    if (!no_ecu)
    {
        seen.protocol[0] = proto;
        seen.protocol[1] = '\0';
        /* the detected (or pinned) protocol's prelude for the rest */
        scan_send_prelude(ap_veh_prelude_for(proto));

        s_scan_phase = PHASE_VIN;
        scan_read_vin(proto, seen.vin);
        scan_responders(&seen);
        ap_veh_fingerprint(seen.ecus, seen.n_ecus, fp);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *supported = NULL;
    uint16_t found = 0;
    bool any_response = false;

    if (root != NULL)
    {
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddStringToObject(root, "protocol", setting);
        cJSON_AddStringToObject(root, "protocol_detected", seen.protocol);
        cJSON_AddStringToObject(root, "vin", seen.vin);
        cJSON_AddStringToObject(root, "fingerprint", fp);
        supported = cJSON_AddArrayToObject(root, "supported");
    }

    if (!no_ecu && supported != NULL)
    {
        s_scan_phase = PHASE_PIDS;
        any_response = scan_pids(supported, &found);
    }

    esp_err_t err = ESP_FAIL;

    if (!any_response || root == NULL)
    {
        snprintf(s_scan_err, sizeof(s_scan_err),
                 (root == NULL) ? "out of memory"
                                : "no ECU response (ignition on?)");
    }
    else
    {
        cJSON_AddNumberToObject(root, "found", found);
        cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

        /* the store first (entry, tables, switch, chip protocol), then
           the result document */
        scan_store(root, supported, &seen, found);

        char *body = cJSON_PrintUnformatted(root);

        if (body != NULL)
        {
            err = filesystem_write(AP_STD_SCAN_PATH, body, strlen(body));
            free(body);
        }

        if (err != ESP_OK)
        {
            snprintf(s_scan_err, sizeof(s_scan_err), "store failed");
        }
    }

    cJSON_Delete(root);
    ap_core_scan_pause(false);

    s_scan_found = found;
    s_scan_ts = (int64_t)time(NULL);
    s_scan_phase = PHASE_IDLE;
    s_scan_state = (err == ESP_OK) ? SCAN_DONE : SCAN_FAILED;
    ap_core_job_release();
    ap_events_scan_done(found);
    ESP_LOGI(TAG, "detection %s: %u PIDs, protocol %s, vin %s, fp %s",
             (err == ESP_OK) ? "done" : "FAILED", found,
             seen.protocol[0] ? seen.protocol : "?",
             seen.vin[0] ? seen.vin : "(none)", fp[0] ? fp : "(none)");

    /* ephemeral tasks escape System Monitor: surface the watermark for
       the stack-audit bench (same net as the dtc job task) */
    ESP_LOGI(TAG, "std scan stack_hw=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t autopid_std_scan_start(void)
{
    if (s_scan_state == SCAN_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* one chip job at a time (test-a-PID / dtc scan / dtc clear) */
    if (!ap_core_job_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_state = SCAN_RUNNING;
    s_scan_phase = PHASE_IDLE;
    s_scan_found = 0;
    s_scan_err[0] = '\0';

    /* INTERNAL stack: the job ends in /data writes (§2) */
    if (xTaskCreateStatic(scan_task, "apid_scan",
                          sizeof(s_scan_stack) / sizeof(StackType_t),
                          NULL, 5, s_scan_stack, &s_scan_tcb) == NULL)
    {
        s_scan_state = SCAN_FAILED;
        snprintf(s_scan_err, sizeof(s_scan_err), "task create failed");
        ap_core_job_release();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

cJSON *ap_std_scan_status_json(void)
{
    static const char *const NAMES[] =
    {
        "idle", "running", "done", "failed",
    };
    static const char *const PHASES[] =
    {
        "idle", "protocol", "vin", "pids",
    };

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(o, "status", NAMES[s_scan_state]);
    cJSON_AddStringToObject(o, "phase", PHASES[s_scan_phase]);
    cJSON_AddNumberToObject(o, "found", s_scan_found);

    if (s_scan_err[0] != '\0')
    {
        cJSON_AddStringToObject(o, "error", s_scan_err);
    }

    if (s_scan_ts != 0)
    {
        cJSON_AddNumberToObject(o, "ts", (double)s_scan_ts);
    }

    size_t size = 0;

    cJSON_AddBoolToObject(o, "stored",
                          filesystem_size(AP_STD_SCAN_PATH, &size)
                                  == ESP_OK &&
                              size > 0);
    return o;
}
