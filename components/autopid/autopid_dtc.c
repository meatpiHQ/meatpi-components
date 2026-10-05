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
 * @file autopid_dtc.c
 * @brief DTC scan / clear engine (TASK_dtc.md §5): the settings-applied
 *        knobs, the scan itself, the conditional clear and the RAM
 *        report. The job task and its triggers live in
 *        autopid_dtc_job.c, the paths in autopid_dtc_obd.c (OBD-II),
 *        autopid_dtc_wwh.c (the same legislated codes of a UDS-dialect
 *        vehicle) and autopid_dtc_uds.c (a manufacturer's 19 02 on one
 *        address pair), the JSON view in autopid_dtc_report.c (split
 *        2026-10-02, 700-line rule).
 *
 * `dtc_protocol` = obd means "the legislated codes": services 03 / 07 / 0A
 * on an OBD-II car, 19 42 / 19 55 on a car whose dialect is `uds`
 * (ISO 27145 / SAE J1979-2), chosen by the vehicle store's current car.
 * Every legislated job starts from the chip baseline (the prelude): a row
 * polled before it may have left its own request header or receive filter
 * behind, and the functional requests of a scan would go to one ECU only.
 *
 * Gates (meatpi 2026-07-08, both default FALSE): `dtc_enabled` for any
 * bus activity, `dtc_allow_clear` additionally for mode 04. Bus access
 * goes through ap_be() and is
 * serialized against polling with ap_core_scan_pause() + against the
 * other one-shot jobs (std scan, test-a-PID) with ap_core_job_acquire().
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "uds_dtc.h"
#include "uds_manager.h"

#include "autopid.h"
#include "autopid_transport.h"
#include "autopid_private.h"
#include "autopid_dtc_engine.h"

static const char *TAG = "autopid";

/* ---- settings-applied knobs -------------------------------------------------- */

static ap_dtc_cfg_t s_cfg;

const ap_dtc_cfg_t *ap_dtc_cfg(void)
{
    return &s_cfg;
}

/* ---- state -------------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

static ap_dtc_report_t s_report EXT_RAM_BSS_ATTR;
static char s_prev_stored[AP_DTC_MAX][AP_DTC_CODE_LEN] EXT_RAM_BSS_ATTR;
static uint8_t s_prev_n;
static bool s_prev_valid;              /* first scan emits no "new" storm */

void ap_dtc_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

/* ---- settings ------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const cJSON *settings,
                     const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    dst[0] = '\0';

    if (cJSON_IsString(v) && v->valuestring != NULL)
    {
        snprintf(dst, cap, "%s", v->valuestring);
    }
}

static bool get_bool(const cJSON *settings, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

void ap_dtc_apply_settings(const cJSON *settings)
{
    const cJSON *v;

    s_cfg.enabled = get_bool(settings, "dtc_enabled", false);
    s_cfg.allow_clear = get_bool(settings, "dtc_allow_clear", false);
    s_cfg.want_pending = get_bool(settings, "dtc_pending", true);
    s_cfg.want_permanent = get_bool(settings, "dtc_permanent", false);
    s_cfg.want_freeze = get_bool(settings, "dtc_freeze", true);

    v = cJSON_GetObjectItemCaseSensitive(settings, "dtc_scan_period_min");
    s_cfg.period_min = cJSON_IsNumber(v) ? (uint32_t)v->valueint : 0;

    copy_str(s_cfg.init, sizeof(s_cfg.init), settings, "dtc_init");
    copy_str(s_cfg.rxheader, sizeof(s_cfg.rxheader), settings, "dtc_rxheader");

    /* UDS path (TASK_dtc §12): protocol obd|uds|auto + one address pair */
    char proto[8];
    char id[12];

    copy_str(proto, sizeof(proto), settings, "dtc_protocol");
    s_cfg.proto = AP_DTC_PROTO_OBD;

    if (strcmp(proto, "uds") == 0)
    {
        s_cfg.proto = AP_DTC_PROTO_UDS;
    }
    else if (strcmp(proto, "auto") == 0)
    {
        s_cfg.proto = AP_DTC_PROTO_AUTO;
    }

    copy_str(id, sizeof(id), settings, "dtc_uds_txid");
    s_cfg.uds_addr.tx_id = (id[0] != '\0')
                           ? (uint32_t)strtoul(id, NULL, 16) : 0x7E0;
    copy_str(id, sizeof(id), settings, "dtc_uds_rxid");
    s_cfg.uds_addr.rx_id = (id[0] != '\0')
                           ? (uint32_t)strtoul(id, NULL, 16) : 0x7E8;
    s_cfg.uds_addr.ext_id = get_bool(settings, "dtc_uds_ext", false);

    v = cJSON_GetObjectItemCaseSensitive(settings, "dtc_uds_mask");
    s_cfg.uds_mask = cJSON_IsNumber(v) ? (uint8_t)v->valueint
                                   : UDS_DTC_STATUS_CONFIRMED;
}

bool ap_dtc_enabled(void)
{
    return s_cfg.enabled;
}

bool ap_dtc_clear_gate_open(void)
{
    return s_cfg.allow_clear;
}

/* ---- the scan ---------------------------------------------------------------------- */

static bool in_set(const char set[][AP_DTC_CODE_LEN], uint8_t n,
                   const char *code)
{
    for (uint8_t i = 0; i < n; i++)
    {
        if (strcmp(set[i], code) == 0)
        {
            return true;
        }
    }

    return false;
}

/** The legislated codes of the current car are asked the WWH-OBD way. */
static bool wwh_car(void)
{
    return autopid_vehicle_dialect() == AP_DIALECT_UDS;
}

/** The current car has no OBD dialect at all: a J1939-only vehicle, whose
 *  codes are heard (DM1), never asked; the chip stays parked. */
static bool j1939_only_car(void)
{
    return autopid_vehicle_dialect() == AP_DIALECT_J1939;
}

const char *ap_dtc_path(void)
{
    if (s_cfg.proto == AP_DTC_PROTO_UDS)
    {
        return "uds";
    }

    if (j1939_only_car())
    {
        return "j1939";
    }

    return wwh_car() ? "wwh" : "obd";
}

/** The OBD-II scan: 0101 (lamp + count per ECU, and the aliveness gate),
 *  then 03 / 07 / 0A and the freeze frame. @return true when an ECU
 *  answered. */
static bool obd_scan(ap_dtc_report_t *r, char *resp, size_t resp_len)
{
    ap_resp_ecu_t *ecus = ap_dtc_obd_ecus();
    bool alive = false;

    /* 01 01: MIL + count from EVERY responder (MIL = OR, count = sum —
       J1979 semantics for a functional scan) */
    if (ap_be()->request("0101", resp, resp_len, AP_DTC_REQ_TIMEOUT) ==
        ESP_OK)
    {
        int n_ecu = ap_resp_to_payloads(resp, ecus, AP_RESP_ECUS_MAX);

        for (int e = 0; e < n_ecu; e++)
        {
            bool mil = false;
            uint8_t cnt = 0;

            if (ap_dtc_parse_mil(ecus[e].payload, ecus[e].len, &mil, &cnt))
            {
                alive = true;
                ap_dtc_report_src(r, ecus[e].header, mil, cnt);
            }
        }
    }

    if (!alive)
    {
        return false;
    }

    (void)ap_dtc_obd_codes("03", 0x43, NULL, r, AP_DTC_KIND_STORED, resp,
                           resp_len);

    if (s_cfg.want_pending)
    {
        (void)ap_dtc_obd_codes("07", 0x47, NULL, r, AP_DTC_KIND_PENDING,
                               resp, resp_len);
    }

    if (s_cfg.want_permanent)
    {
        (void)ap_dtc_obd_codes("0A", 0x4A, NULL, r, AP_DTC_KIND_PERMANENT,
                               resp, resp_len);
    }

    /* freeze frame while the headers-on window is still open — only when
       something is stored (no codes = no frame) */
    if (s_cfg.want_freeze && r->n_stored > 0)
    {
        ap_dtc_obd_freeze(r, resp, resp_len);
    }

    snprintf(r->protocol, sizeof(r->protocol), "obd");
    r->valid = true;
    return true;
}

/** Runs in the job task (scan) — poller already paused by the caller. */
void ap_dtc_run_scan(char *resp, size_t resp_len)
{
    /* the job task only (one job at a time): ~5 KB that is not its stack */
    static ap_dtc_report_t s_scan EXT_RAM_BSS_ATTR;
    ap_dtc_report_t *r = &s_scan;
    bool alive = false;

    memset(r, 0, sizeof(*r));

    if (s_cfg.proto != AP_DTC_PROTO_UDS && !j1939_only_car())
    {
        /* from the baseline (see the file header), then the user's init */
        ap_runner_restore_baseline();
        ap_dtc_obd_prep(resp, resp_len);

        alive = wwh_car() ? ap_dtc_wwh_scan(r, resp, resp_len)
                          : obd_scan(r, resp, resp_len);

        ap_dtc_obd_done(resp, resp_len);
    }

    /* a J1939 network (alone, or beside the OBD dialect of an EU truck):
       the active codes every controller broadcasts, from the listener's
       store; in active mode the previously active ones are asked for */
    if (s_cfg.proto != AP_DTC_PROTO_UDS && autopid_vehicle_j1939())
    {
        alive = ap_dtc_j1939_scan(r) || alive;
    }

    if (!alive && s_cfg.proto != AP_DTC_PROTO_OBD && !j1939_only_car())
    {
        /* forced `uds`, or `auto` falling back after a silent scan */
        if (ap_dtc_uds_scan(r))
        {
            alive = true;
            r->valid = true;
        }

        /* the UDS transport addressed the chip its own way (header,
           receive filter, reply wait): the poller's prelude is due again */
        ap_runner_baseline_invalidate();
    }

    if (!alive && j1939_only_car())
    {
        snprintf(r->error, sizeof(r->error),
                 "no DM1 heard (J1939 listener %s)",
                 ap_j1939_listening() ? "up" : "off");
    }
    else if (!alive)
    {
        snprintf(r->error, sizeof(r->error), "no ECU response (ignition on?)");
    }

    r->ts_us = esp_timer_get_time();

    time_t now = time(NULL);

    r->ts_epoch = (now > 1577836800) ? (int64_t)now : 0; /* 2020 floor */

    /* diff against the previous scan (first scan = no "new" storm) */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (r->valid && s_prev_valid)
    {
        for (uint8_t i = 0; i < r->n_stored; i++)
        {
            if (!in_set(s_prev_stored, s_prev_n, r->stored[i]))
            {
                memcpy(r->new_codes[r->n_new], r->stored[i],
                       AP_DTC_CODE_LEN);
                r->n_new++;
            }
        }
    }

    if (r->valid)
    {
        memcpy(s_prev_stored, r->stored, sizeof(s_prev_stored));
        s_prev_n = r->n_stored;
        s_prev_valid = true;
        s_report = *r;
    }
    else
    {
        /* keep the last good report, surface the error */
        memcpy(s_report.error, r->error, sizeof(s_report.error));
        s_report.error[sizeof(s_report.error) - 1] = '\0';
    }

    xSemaphoreGive(s_lock);

    for (uint8_t i = 0; i < r->n_new; i++)
    {
        ap_events_dtc_new_code(r->new_codes[i], "stored", r->mil);
    }

    ap_events_dtc_scan(r, r->valid);
    ESP_LOGI(TAG, "dtc scan %s (%s): %u stored, %u pending, %u permanent, "
                  "%u new, %u ECUs, MIL %s",
             r->valid ? "done" : "FAILED",
             r->protocol[0] ? r->protocol : "-", r->n_stored, r->n_pending,
             r->n_permanent, r->n_new, r->n_ecus, r->mil ? "ON" : "off");
}

/* ---- the clear (sync; HTTP/CLI/script/job contexts — NEVER the event
 *      dispatcher) ------------------------------------------------------------------- */

/** The stored codes on the bus NOW, by the path in use.
 *  @return their count (0 when nothing usable answered). */
static uint8_t clear_read_present(bool use_uds, bool use_wwh,
                                  char present[][AP_DTC_CODE_LEN],
                                  ap_dtc_report_t *scratch, char *resp,
                                  size_t resp_len)
{
    int n;

    if (use_uds)
    {
        n = ap_dtc_uds_codes(s_cfg.uds_mask, present, NULL);
    }
    else if (use_wwh)
    {
        memset(scratch, 0, sizeof(*scratch));
        n = ap_dtc_wwh_codes(AP_DTC_KIND_STORED, scratch, resp, resp_len);
        memcpy(present, scratch->stored, sizeof(scratch->stored));
    }
    else
    {
        n = (int)ap_dtc_obd_codes("03", 0x43, present, NULL,
                                  AP_DTC_KIND_STORED, resp, resp_len);
    }

    return (n > 0) ? (uint8_t)n : 0;
}

/** After a clear: the stored items that are gone leave the detail too. */
static void report_drop_items(ap_dtc_report_t *r,
                              const char left[][AP_DTC_CODE_LEN],
                              uint8_t n_left)
{
    uint8_t w = 0;

    for (uint8_t i = 0; i < r->n_items; i++)
    {
        const ap_dtc_item_t *it = &r->items[i];

        if (it->kind == AP_DTC_KIND_STORED &&
            !in_set(left, n_left, it->code))
        {
            continue;
        }

        r->items[w++] = *it;
    }

    r->n_items = w;
}

esp_err_t ap_dtc_clear(const char *codes, const char *mode, bool *cleared,
                       uint8_t *before, uint8_t *after, char *err,
                       size_t err_len)
{
    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* under the job flag */
    /* ... as are these two: the codes present now, and the scratch report
       the WWH path reads them into */
    static char present[AP_DTC_MAX][AP_DTC_CODE_LEN] EXT_RAM_BSS_ATTR;
    static ap_dtc_report_t s_now EXT_RAM_BSS_ATTR;

    if (cleared != NULL)
    {
        *cleared = false;
    }

    if (before != NULL)
    {
        *before = 0;
    }

    if (after != NULL)
    {
        *after = 0;
    }

    if (!s_cfg.enabled || !s_cfg.allow_clear)
    {
        snprintf(err, err_len, !s_cfg.enabled ? "dtc_enabled is off"
                                          : "dtc_allow_clear is off");
        return ESP_ERR_NOT_ALLOWED;
    }

    ap_dtc_clear_mode_t m = ap_dtc_clear_mode_parse(mode, codes);

    if (m == AP_DTC_CLEAR_INVALID)
    {
        snprintf(err, err_len, "mode must be always|if_any|if_only");
        return ESP_ERR_INVALID_ARG;
    }

    if (j1939_only_car() && s_cfg.proto != AP_DTC_PROTO_UDS)
    {
        /* DM11 / DM3 on the network, no chip job: active mode only (a
           listener gets the 403 from there) */
        return ap_dtc_j1939_clear(codes, m, cleared, before, after, err,
                                  err_len);
    }

    if (!ap_guard_job_ok(err, err_len) || !ap_dtc_obd_init_ok())
    {
        return ESP_ERR_NOT_SUPPORTED; /* ap_guard_last_reason() says why */
    }

    if (!ap_core_job_acquire())
    {
        snprintf(err, err_len, "another chip job is running");
        return ESP_ERR_INVALID_STATE;
    }

    /* UDS path when forced, or when `auto`'s last scan answered on UDS
       (14 groupOfDTC = the only true per-code clear; TASK_dtc §12) */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool use_uds = (s_cfg.proto == AP_DTC_PROTO_UDS) ||
                   (s_cfg.proto == AP_DTC_PROTO_AUTO &&
                    strcmp(s_report.protocol, "uds") == 0);

    xSemaphoreGive(s_lock);

    /* ... else the legislated path of the car's dialect */
    bool use_wwh = !use_uds && wwh_car();

    ap_core_scan_pause(true);

    if (!use_uds)
    {
        ap_runner_restore_baseline();   /* as a scan: from the baseline */
        ap_dtc_obd_prep(resp, sizeof(resp));
    }

    /* condition evaluates against the CURRENT codes, never a stale scan */
    uint8_t n_present = clear_read_present(use_uds, use_wwh, present, &s_now,
                                           resp, sizeof(resp));
    esp_err_t result = ESP_OK;
    bool did_clear = false;

    if (before != NULL)
    {
        *before = n_present;
    }

    if (ap_dtc_clear_allowed(present, n_present, codes, m))
    {
        bool ok;

        if (use_uds)
        {
            ok = ap_dtc_uds_clear(codes);

            if (!ok)
            {
                snprintf(err, err_len, "UDS 14 not confirmed");
            }
        }
        else if (use_wwh)
        {
            ok = ap_dtc_wwh_clear(resp, sizeof(resp));

            if (!ok)
            {
                snprintf(err, err_len, "14 FFFF33 not confirmed");
            }
        }
        else
        {
            uint8_t payload[AP_PAYLOAD_MAX];
            size_t plen = 0;

            ok = ap_be()->request("04", resp, sizeof(resp),
                                  AP_DTC_REQ_TIMEOUT) == ESP_OK &&
                 ap_resp_to_payload(resp, payload, sizeof(payload),
                                    &plen) == ESP_OK &&
                 plen >= 1 && payload[0] == 0x44;

            if (!ok)
            {
                snprintf(err, err_len, "mode 04 not confirmed");
            }
        }

        if (!ok)
        {
            result = ESP_FAIL;
        }
        else
        {
            did_clear = true;

            uint8_t n_after = clear_read_present(use_uds, use_wwh, present,
                                                 &s_now, resp, sizeof(resp));

            if (after != NULL)
            {
                *after = n_after;
            }

            /* reflect the wipe in the report + diff memory (no "new"
             * storm when codes come back later — they ARE new then) */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(s_prev_stored, present, sizeof(s_prev_stored));
            s_prev_n = n_after;
            s_prev_valid = true;
            memcpy(s_report.stored, present, sizeof(s_report.stored));
            s_report.n_stored = n_after;
            s_report.n_new = 0;
            report_drop_items(&s_report, present, n_after);
            xSemaphoreGive(s_lock);
        }
    }

    if (!use_uds)
    {
        ap_dtc_obd_done(resp, sizeof(resp));
    }
    else
    {
        ap_runner_baseline_invalidate();    /* the UDS transport's setup */
    }

    ap_core_scan_pause(false);
    ap_core_job_release();

    if (cleared != NULL)
    {
        *cleared = did_clear;
    }

    ap_events_dtc_clear(result == ESP_OK, did_clear, n_present,
                        (after != NULL) ? *after : 0);
    ESP_LOGI(TAG, "dtc clear: %s (before %u)",
             did_clear ? "CLEARED" : "condition not met / failed",
             n_present);
    return result;
}

/* ---- report access --------------------------------------------------------------------- */

const ap_dtc_report_t *ap_dtc_report_lock(void)
{
    ap_dtc_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_report;
}

void ap_dtc_report_unlock(void)
{
    xSemaphoreGive(s_lock);
}

void ap_dtc_report_brief(bool *valid, bool *mil, uint8_t *mil_count,
                         uint8_t *n_stored)
{
    const ap_dtc_report_t *r = ap_dtc_report_lock();

    if (valid != NULL)
    {
        *valid = r->valid;
    }

    if (mil != NULL)
    {
        *mil = r->mil;
    }

    if (mil_count != NULL)
    {
        *mil_count = r->mil_count;
    }

    if (n_stored != NULL)
    {
        *n_stored = r->n_stored;
    }

    ap_dtc_report_unlock();
}

void ap_dtc_report_stored_csv(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }

    out[0] = '\0';

    const ap_dtc_report_t *r = ap_dtc_report_lock();
    size_t w = 0;

    for (uint8_t i = 0; i < r->n_stored && w + AP_DTC_CODE_LEN + 1 < out_len;
         i++)
    {
        w += (size_t)snprintf(out + w, out_len - w, "%s%s",
                              (i > 0) ? "," : "", r->stored[i]);
    }

    ap_dtc_report_unlock();
}
