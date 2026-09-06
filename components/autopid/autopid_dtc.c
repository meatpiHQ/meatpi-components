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
 * @brief DTC scan / clear engine (TASK_dtc.md §5): the async scan job
 *        (std-scan pattern), the conditional mode-04 clear, the periodic
 *        due-check the poller calls, and the RAM report.
 *
 * Gates (meatpi 2026-07-08, both default FALSE): `dtc_enabled` for any
 * bus activity, `dtc_allow_clear` additionally for mode 04. Bus access
 * goes through ap_be() and is
 * serialized against polling with ap_core_scan_pause() + against the
 * other one-shot jobs (std scan, test-a-PID) with ap_core_job_acquire().
 *
 * The job task stack is PSRAM — DTC jobs never touch the filesystem
 * (report is RAM-only by design; history = event rules -> logger).
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

static const char *TAG = "autopid";

/* first request after a protocol prelude can sit in SEARCHING — the std
 * scan learned the same lesson (AP_SCAN_REQ_TIMEOUT 10 s) */
#define DTC_REQ_TIMEOUT   pdMS_TO_TICKS(5000)
#define DTC_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

/* ---- settings-applied knobs -------------------------------------------------- */

static bool     s_enabled;
static bool     s_allow_clear;
static uint32_t s_period_min;
static bool     s_want_pending;
static bool     s_want_permanent;
static bool     s_want_freeze;
static char     s_init[AP_INIT_LEN];
static char     s_rxheader[AP_HDR_LEN];

/* UDS path (TASK_dtc §12) */
typedef enum
{
    DTC_PROTO_OBD = 0,
    DTC_PROTO_UDS,
    DTC_PROTO_AUTO,             /* OBD first, UDS when no ECU answers    */
} dtc_proto_t;

static dtc_proto_t s_proto;
static uds_addr_t  s_uds_addr;
static uint8_t     s_uds_mask;

/* ---- state -------------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

static ap_dtc_report_t s_report EXT_RAM_BSS_ATTR;
static char s_prev_stored[AP_DTC_MAX][AP_DTC_CODE_LEN] EXT_RAM_BSS_ATTR;
static uint8_t s_prev_n;
static bool s_prev_valid;              /* first scan emits no "new" storm */

static volatile bool s_busy;           /* a dtc job (scan or clear) runs  */
static int64_t s_last_scan_us;         /* periodic anchor (job START)     */

/* one-shot job task (scan, or a rule-queued clear) */
typedef enum
{
    DTC_JOB_SCAN = 0,
    DTC_JOB_CLEAR,
} dtc_job_t;

static dtc_job_t   s_job;
static char        s_job_codes[192];
static char        s_job_mode[12];
static StaticTask_t s_job_tcb;         /* internal: FreeRTOS object */
/* 16 KB (StackType_t = BYTES on xtensa): ap_resp_to_payload(s)' line
   table alone is ~5.6 KB of frame — 6144 overflowed the moment the
   scan grew the multi-ECU path (silent PSRAM-bss scribble; found
   2026-07-22 when an internal-RAM stack turned it into a panic) */
static StackType_t  s_job_stack[16384] EXT_RAM_BSS_ATTR; /* no fs I/O */

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

    s_enabled = get_bool(settings, "dtc_enabled", false);
    s_allow_clear = get_bool(settings, "dtc_allow_clear", false);
    s_want_pending = get_bool(settings, "dtc_pending", true);
    s_want_permanent = get_bool(settings, "dtc_permanent", false);
    s_want_freeze = get_bool(settings, "dtc_freeze", true);

    v = cJSON_GetObjectItemCaseSensitive(settings, "dtc_scan_period_min");
    s_period_min = cJSON_IsNumber(v) ? (uint32_t)v->valueint : 0;

    copy_str(s_init, sizeof(s_init), settings, "dtc_init");
    copy_str(s_rxheader, sizeof(s_rxheader), settings, "dtc_rxheader");

    /* UDS path (TASK_dtc §12): protocol obd|uds|auto + one address pair */
    char proto[8];
    char id[12];

    copy_str(proto, sizeof(proto), settings, "dtc_protocol");
    s_proto = DTC_PROTO_OBD;

    if (strcmp(proto, "uds") == 0)
    {
        s_proto = DTC_PROTO_UDS;
    }
    else if (strcmp(proto, "auto") == 0)
    {
        s_proto = DTC_PROTO_AUTO;
    }

    copy_str(id, sizeof(id), settings, "dtc_uds_txid");
    s_uds_addr.tx_id = (id[0] != '\0')
                           ? (uint32_t)strtoul(id, NULL, 16) : 0x7E0;
    copy_str(id, sizeof(id), settings, "dtc_uds_rxid");
    s_uds_addr.rx_id = (id[0] != '\0')
                           ? (uint32_t)strtoul(id, NULL, 16) : 0x7E8;
    s_uds_addr.ext_id = get_bool(settings, "dtc_uds_ext", false);

    v = cJSON_GetObjectItemCaseSensitive(settings, "dtc_uds_mask");
    s_uds_mask = cJSON_IsNumber(v) ? (uint8_t)v->valueint
                                   : UDS_DTC_STATUS_CONFIRMED;
}

bool ap_dtc_enabled(void)
{
    return s_enabled;
}

bool ap_dtc_clear_gate_open(void)
{
    return s_allow_clear;
}

bool ap_dtc_busy(void)
{
    return s_busy;
}

/* ---- chip choreography ----------------------------------------------------------- */

/** Optional prep: dtc_init commands (';'-separated) + ATCRA rxheader.
 *  Best-effort — a failed init command doesn't abort the scan. */
static void dtc_chip_prep(char *resp, size_t resp_len)
{
    const char *p = s_init;

    while (*p != '\0')
    {
        const char *sep = strchr(p, ';');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char one[AP_INIT_LEN];

        if (len > 0 && len < sizeof(one))
        {
            memcpy(one, p, len);
            one[len] = '\0';
            ap_init_sanitize(one); /* spare the chip's EEPROM */
            (void)ap_be()->request(one, resp, resp_len, DTC_INIT_TIMEOUT);
        }

        p += len + ((sep != NULL) ? 1 : 0);
    }

    /* headers ON for the scan window (after the user's init so it can't
       be undone by accident): responses carry CAN ids, which is what
       lets a functional scan keep EVERY responding ECU apart */
    (void)ap_be()->request("ATH1", resp, resp_len, DTC_INIT_TIMEOUT);

    if (s_rxheader[0] != '\0')
    {
        char cra[AP_HDR_LEN + 8];

        snprintf(cra, sizeof(cra), "ATCRA%s", s_rxheader);
        (void)ap_be()->request(cra, resp, resp_len, DTC_INIT_TIMEOUT);
    }
}

static void dtc_chip_done(char *resp, size_t resp_len)
{
    (void)ap_be()->request("ATH0", resp, resp_len, DTC_INIT_TIMEOUT);

    if (s_rxheader[0] != '\0')
    {
        (void)ap_be()->request("ATCRA", resp, resp_len, DTC_INIT_TIMEOUT);
    }
}

/* one per-ECU assembly buffer — scan/clear jobs run one at a time */
static ap_resp_ecu_t s_ecus[AP_RESP_ECUS_MAX] EXT_RAM_BSS_ATTR;

/** Request @p cmd and parse its DTC list from EVERY responding ECU
 *  (functional scans hit several; codes are union-merged, duplicates
 *  dropped). A request/parse failure counts as zero codes (mode 07/0A
 *  on a no-pending ECU answers NO DATA; the 0101 probe already proved
 *  an ECU is alive). Cross-talk and negative responses too. */
static uint8_t dtc_request_codes(const char *cmd, uint8_t svc,
                                 char out[][AP_DTC_CODE_LEN],
                                 char *resp, size_t resp_len)
{
    if (ap_be()->request(cmd, resp, resp_len, DTC_REQ_TIMEOUT) != ESP_OK)
    {
        return 0;
    }

    int n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);
    uint8_t n = 0;

    for (int e = 0; e < n_ecu; e++)
    {
        if (!ap_payload_matches_cmd(cmd, s_ecus[e].payload,
                                    s_ecus[e].len))
        {
            continue;
        }

        char codes[AP_DTC_MAX][AP_DTC_CODE_LEN];
        int c = ap_dtc_parse_codes(s_ecus[e].payload, s_ecus[e].len,
                                   svc, codes, AP_DTC_MAX);

        if (c > 0)
        {
            n = ap_dtc_merge_codes(out, n, AP_DTC_MAX, codes,
                                   (uint8_t)c);
        }
    }

    return n;
}

/* ---- freeze frame (mode 02 frame 0 — TASK_dtc §14) --------------------------------- */

/* Curated value PIDs — the classic freeze-frame set, bounded scan cost.
 * The supported-PID bitmaps (02 00/20/40) prune this to what the ECU
 * actually stores; a silent bitmap falls back to blind probing (an
 * unsupported PID just answers NO DATA = zero decoded values). */
static const uint8_t FRZ_PIDS[] =
{
    0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x2F, 0x33, 0x42, 0x46,
};

/** The freeze ECU's payload from the last request, or NULL. Headers are
 *  ON during the scan window, so responders are told apart by CAN id;
 *  UINT32_MAX (headers off despite ATH1) accepts the first entry. */
static const ap_resp_ecu_t *frz_ecu_payload(int n_ecu, uint32_t want)
{
    for (int e = 0; e < n_ecu; e++)
    {
        if (want == UINT32_MAX || s_ecus[e].header == want ||
            s_ecus[e].header == UINT32_MAX)
        {
            return &s_ecus[e];
        }
    }

    return NULL;
}

/** Capture frame 0 into @p r: DTCFRZF (which DTC froze it, and which
 *  ECU answered — the functional query can hit several; first responder
 *  with a frame wins, multi-ECU freeze = v2), then the curated PIDs
 *  decoded via the standard table. Best-effort throughout — a scan
 *  never fails because the freeze frame is unreadable. */
static void dtc_run_freeze(ap_dtc_report_t *r, char *resp, size_t resp_len)
{
    if (ap_be()->request("020200", resp, resp_len, DTC_REQ_TIMEOUT) !=
        ESP_OK)
    {
        return;
    }

    int n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);

    for (int e = 0; e < n_ecu && !r->frz_present; e++)
    {
        if (ap_frz_dtc(s_ecus[e].payload, s_ecus[e].len, r->frz_dtc))
        {
            r->frz_present = true;
            r->frz_ecu = s_ecus[e].header;
        }
    }

    if (!r->frz_present)
    {
        return;         /* codes stored but no frame (or NRC) — fine */
    }

    /* supported bitmaps for the ranges the curated list spans */
    uint32_t bm[3] = { 0, 0, 0 };
    bool bm_ok[3] = { false, false, false };

    for (int g = 0; g < 3; g++)
    {
        char cmd[8];

        snprintf(cmd, sizeof(cmd), "02%02X00", g * 0x20);

        if (ap_be()->request(cmd, resp, resp_len, DTC_REQ_TIMEOUT) !=
            ESP_OK)
        {
            continue;
        }

        n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);

        const ap_resp_ecu_t *ecu = frz_ecu_payload(n_ecu, r->frz_ecu);

        if (ecu != NULL)
        {
            bm_ok[g] = ap_frz_bitmap(ecu->payload, ecu->len,
                                     (uint8_t)(g * 0x20), &bm[g]);
        }
    }

    for (size_t i = 0;
         i < sizeof(FRZ_PIDS) && r->n_frz < AP_FRZ_MAX; i++)
    {
        uint8_t pid = FRZ_PIDS[i];
        int g = (pid - 1) / 0x20;

        /* bitmap bit: MSB of byte 0 = PID base+1 */
        if (bm_ok[g] &&
            ((bm[g] >> (31 - ((pid - 1) % 0x20))) & 1u) == 0)
        {
            continue;
        }

        char cmd[8];

        snprintf(cmd, sizeof(cmd), "02%02X00", pid);

        if (ap_be()->request(cmd, resp, resp_len, DTC_REQ_TIMEOUT) !=
            ESP_OK)
        {
            continue;
        }

        n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);

        const ap_resp_ecu_t *ecu = frz_ecu_payload(n_ecu, r->frz_ecu);

        if (ecu != NULL && ecu->len > 1 && ecu->payload[1] == pid)
        {
            r->n_frz = (uint8_t)ap_frz_decode(ecu->payload, ecu->len,
                                              r->frz, r->n_frz,
                                              AP_FRZ_MAX);
        }
    }

    ESP_LOGI(TAG, "dtc freeze frame: %s from %lX, %u values",
             r->frz_dtc, (unsigned long)r->frz_ecu, r->n_frz);
}

/* ---- the UDS path (TASK_dtc §12) --------------------------------------------------- */

/** One `19 02 <mask>` transaction -> formatted codes ("P0420" /
 *  "P0420-08"). @return count, or -1 = no usable answer (transport
 *  down / timeout / negative response). */
static int dtc_uds_codes(uint8_t mask, char out[][AP_DTC_CODE_LEN])
{
    static uint8_t resp[8 + AP_DTC_MAX * 4] EXT_RAM_BSS_ATTR; /* job    */
    uint8_t req[3];
    size_t rlen = 0;
    uds_result_t res;
    size_t n = uds_dtc_req_by_status(mask, req);

    if (uds_request(&s_uds_addr, req, n, resp, sizeof(resp), &rlen,
                    NULL, &res) != ESP_OK || res.negative)
    {
        return -1;
    }

    uds_dtc_t recs[AP_DTC_MAX];
    int cnt = uds_dtc_parse_list(resp, rlen, NULL, recs, AP_DTC_MAX);

    if (cnt < 0)
    {
        return -1;
    }

    for (int i = 0; i < cnt; i++)
    {
        uds_dtc_format(recs[i].hi, recs[i].mid, recs[i].ftb, out[i]);
    }

    return cnt;
}

/** One 14 <group> transaction. NULL group = FFFFFF (all). */
static bool dtc_uds_clear_group(const uint8_t group[3])
{
    uint8_t req[4];
    uint8_t resp[8];
    size_t rlen = 0;
    uds_result_t res;
    size_t n = uds_dtc_req_clear(group, req);

    return uds_request(&s_uds_addr, req, n, resp, sizeof(resp), &rlen,
                       NULL, &res) == ESP_OK &&
           !res.negative && uds_dtc_clear_ok(resp, rlen);
}

/** Clear via UDS: a CSV codes list -> one 14 per code (TRUE selective
 *  clear — the thing OBD mode 04 can't do); empty list -> group
 *  FFFFFF. ALL calls must confirm. */
static bool dtc_uds_clear(const char *codes)
{
    if (codes == NULL || codes[0] == '\0')
    {
        return dtc_uds_clear_group(NULL);
    }

    const char *p = codes;

    while (*p != '\0')
    {
        while (*p == ' ' || *p == ',')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        char one[AP_DTC_CODE_LEN];
        size_t n = 0;

        while (p[n] != '\0' && p[n] != ',' && p[n] != ' ' &&
               n < sizeof(one) - 1)
        {
            one[n] = p[n];
            n++;
        }

        one[n] = '\0';
        p += n;

        uint8_t group[3];

        if (!uds_dtc_unformat(one, &group[0], &group[1], &group[2]) ||
            !dtc_uds_clear_group(group))
        {
            return false;
        }
    }

    return true;
}

/** UDS scan: stored = 19 02 <mask>, pending = 19 02 pendingDTC-bit.
 *  Permanent has no UDS equivalent (OBD-only concept) and MIL is not
 *  carried by 0x19 — both stay zero under forced `uds` (documented,
 *  TASK_dtc §12). @return true when the ECU answered. */
static bool dtc_run_scan_uds(ap_dtc_report_t *r)
{
    int n = dtc_uds_codes(s_uds_mask, r->stored);

    if (n < 0)
    {
        return false;
    }

    r->n_stored = (uint8_t)n;

    if (s_want_pending)
    {
        n = dtc_uds_codes(UDS_DTC_STATUS_PENDING, r->pending);
        r->n_pending = (n > 0) ? (uint8_t)n : 0;
    }

    r->n_ecus = 1;              /* one address pair in v1                */
    snprintf(r->protocol, sizeof(r->protocol), "uds");
    return true;
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

/** Runs in the job task (scan) — poller already paused by the caller. */
static void dtc_run_scan(char *resp, size_t resp_len)
{
    ap_dtc_report_t r;

    memset(&r, 0, sizeof(r));

    bool alive = false;

    if (s_proto != DTC_PROTO_UDS)
    {
        dtc_chip_prep(resp, resp_len);

        /* 01 01: MIL + count from EVERY responder (MIL = OR, count =
           sum — J1979 semantics for a functional scan), and the
           ECU-aliveness gate for the OBD scan */
        if (ap_be()->request("0101", resp, resp_len, DTC_REQ_TIMEOUT) ==
            ESP_OK)
        {
            int n_ecu = ap_resp_to_payloads(resp, s_ecus,
                                            AP_RESP_ECUS_MAX);
            unsigned count_sum = 0;

            for (int e = 0; e < n_ecu; e++)
            {
                bool mil = false;
                uint8_t cnt = 0;

                if (ap_dtc_parse_mil(s_ecus[e].payload, s_ecus[e].len,
                                     &mil, &cnt))
                {
                    alive = true;
                    r.mil |= mil;
                    count_sum += cnt;
                    r.n_ecus++;
                }
            }

            r.mil_count = (count_sum > UINT8_MAX) ? UINT8_MAX
                                                  : (uint8_t)count_sum;
        }

        if (alive)
        {
            r.n_stored = dtc_request_codes("03", 0x43, r.stored, resp,
                                           resp_len);

            if (s_want_pending)
            {
                r.n_pending = dtc_request_codes("07", 0x47, r.pending,
                                                resp, resp_len);
            }

            if (s_want_permanent)
            {
                r.n_permanent = dtc_request_codes("0A", 0x4A,
                                                  r.permanent, resp,
                                                  resp_len);
            }

            /* freeze frame while the headers-on window is still open —
               only when something is stored (no codes = no frame) */
            if (s_want_freeze && r.n_stored > 0)
            {
                dtc_run_freeze(&r, resp, resp_len);
            }

            snprintf(r.protocol, sizeof(r.protocol), "obd");
            r.valid = true;
        }

        dtc_chip_done(resp, resp_len);
    }

    if (!alive && s_proto != DTC_PROTO_OBD)
    {
        /* forced `uds`, or `auto` falling back after a silent OBD scan */
        if (dtc_run_scan_uds(&r))
        {
            alive = true;
            r.valid = true;
        }
    }

    if (!alive)
    {
        snprintf(r.error, sizeof(r.error), "no ECU response (ignition on?)");
    }

    r.ts_us = esp_timer_get_time();

    time_t now = time(NULL);

    r.ts_epoch = (now > 1577836800) ? (int64_t)now : 0; /* 2020 floor */

    /* diff against the previous scan (first scan = no "new" storm) */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (r.valid && s_prev_valid)
    {
        for (uint8_t i = 0; i < r.n_stored; i++)
        {
            if (!in_set(s_prev_stored, s_prev_n, r.stored[i]))
            {
                memcpy(r.new_codes[r.n_new], r.stored[i],
                       AP_DTC_CODE_LEN);
                r.n_new++;
            }
        }
    }

    if (r.valid)
    {
        memcpy(s_prev_stored, r.stored, sizeof(s_prev_stored));
        s_prev_n = r.n_stored;
        s_prev_valid = true;
        s_report = r;
    }
    else
    {
        /* keep the last good report, surface the error */
        snprintf(s_report.error, sizeof(s_report.error), "%s", r.error);
    }

    xSemaphoreGive(s_lock);

    for (uint8_t i = 0; i < r.n_new; i++)
    {
        ap_events_dtc_new_code(r.new_codes[i], "stored", r.mil);
    }

    ap_events_dtc_scan(&r, r.valid);
    ESP_LOGI(TAG, "dtc scan %s: %u stored, %u pending, %u permanent, "
                  "%u new, MIL %s",
             r.valid ? "done" : "FAILED", r.n_stored, r.n_pending,
             r.n_permanent, r.n_new, r.mil ? "ON" : "off");
}

/* ---- the clear (sync; HTTP/CLI/script/job contexts — NEVER the event
 *      dispatcher) ------------------------------------------------------------------- */

esp_err_t ap_dtc_clear(const char *codes, const char *mode, bool *cleared,
                       uint8_t *before, uint8_t *after, char *err,
                       size_t err_len)
{
    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* under the job flag */

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

    if (!s_enabled || !s_allow_clear)
    {
        snprintf(err, err_len, !s_enabled ? "dtc_enabled is off"
                                          : "dtc_allow_clear is off");
        return ESP_ERR_NOT_ALLOWED;
    }

    ap_dtc_clear_mode_t m = ap_dtc_clear_mode_parse(mode, codes);

    if (m == AP_DTC_CLEAR_INVALID)
    {
        snprintf(err, err_len, "mode must be always|if_any|if_only");
        return ESP_ERR_INVALID_ARG;
    }

    if (!ap_core_job_acquire())
    {
        snprintf(err, err_len, "another chip job is running");
        return ESP_ERR_INVALID_STATE;
    }

    /* UDS path when forced, or when `auto`'s last scan answered on UDS
       (14 groupOfDTC = the only true per-code clear; TASK_dtc §12) */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool use_uds = (s_proto == DTC_PROTO_UDS) ||
                   (s_proto == DTC_PROTO_AUTO &&
                    strcmp(s_report.protocol, "uds") == 0);

    xSemaphoreGive(s_lock);

    ap_core_scan_pause(true);

    if (!use_uds)
    {
        dtc_chip_prep(resp, sizeof(resp));
    }

    /* condition evaluates against the CURRENT codes, never a stale scan */
    char present[AP_DTC_MAX][AP_DTC_CODE_LEN];
    int n_read = use_uds
        ? dtc_uds_codes(s_uds_mask, present)
        : (int)dtc_request_codes("03", 0x43, present, resp,
                                 sizeof(resp));
    uint8_t n_present = (n_read > 0) ? (uint8_t)n_read : 0;
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
            ok = dtc_uds_clear(codes);

            if (!ok)
            {
                snprintf(err, err_len, "UDS 14 not confirmed");
            }
        }
        else
        {
            uint8_t payload[AP_PAYLOAD_MAX];
            size_t plen = 0;

            ok = ap_be()->request("04", resp, sizeof(resp),
                                  DTC_REQ_TIMEOUT) == ESP_OK &&
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

            n_read = use_uds
                ? dtc_uds_codes(s_uds_mask, present)
                : (int)dtc_request_codes("03", 0x43, present, resp,
                                         sizeof(resp));

            uint8_t n_after = (n_read > 0) ? (uint8_t)n_read : 0;

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
            xSemaphoreGive(s_lock);
        }
    }

    if (!use_uds)
    {
        dtc_chip_done(resp, sizeof(resp));
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

/* ---- the job task + triggers -------------------------------------------------------- */

static void dtc_job_task(void *arg)
{
    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* one job at a time */

    (void)arg;

    if (s_job == DTC_JOB_SCAN)
    {
        ap_core_scan_pause(true);
        dtc_run_scan(resp, sizeof(resp));
        ap_core_scan_pause(false);
        ap_core_job_release();
    }
    else
    {
        /* rule-queued clear: the job flag was NOT pre-acquired —
         * ap_dtc_clear() takes it itself; results travel by event */
        char err[48];

        (void)ap_dtc_clear(s_job_codes, s_job_mode, NULL, NULL, NULL,
                           err, sizeof(err));
    }

    /* ephemeral tasks escape System Monitor — surface the watermark so
       the stack-audit bench (and any log reader) sees how close this
       job came to the 2026-07-22 silent-overflow cliff */
    ESP_LOGI(TAG, "dtc job stack_hw=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t dtc_job_spawn(dtc_job_t job)
{
    s_job = job;

    if (xTaskCreateStatic(dtc_job_task, "apid_dtc",
                          sizeof(s_job_stack) / sizeof(s_job_stack[0]),
                          NULL, 5, s_job_stack, &s_job_tcb) == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ap_dtc_scan_start(void)
{
    if (!s_enabled)
    {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (s_busy || !ap_core_job_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    s_last_scan_us = esp_timer_get_time();

    esp_err_t err = dtc_job_spawn(DTC_JOB_SCAN);

    if (err != ESP_OK)
    {
        s_busy = false;
        ap_core_job_release();
    }

    return err;
}

esp_err_t ap_dtc_clear_queue(const char *codes, const char *mode)
{
    if (!s_enabled || !s_allow_clear)
    {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (s_busy)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    snprintf(s_job_codes, sizeof(s_job_codes), "%s",
             (codes != NULL) ? codes : "");
    snprintf(s_job_mode, sizeof(s_job_mode), "%s",
             (mode != NULL) ? mode : "");

    esp_err_t err = dtc_job_spawn(DTC_JOB_CLEAR);

    if (err != ESP_OK)
    {
        s_busy = false;
    }

    return err;
}

void ap_dtc_periodic_check(void)
{
    if (!s_enabled || s_period_min == 0 || s_busy)
    {
        return;
    }

    int64_t period_us = (int64_t)s_period_min * 60 * 1000000;

    if (esp_timer_get_time() - s_last_scan_us >= period_us)
    {
        (void)ap_dtc_scan_start(); /* busy/collision = try next tick */
    }
}

/* ---- report access --------------------------------------------------------------------- */

esp_err_t ap_dtc_report_get(ap_dtc_report_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ap_dtc_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_report;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void add_code_array(cJSON *o, const char *key,
                           const char codes[][AP_DTC_CODE_LEN], uint8_t n)
{
    cJSON *arr = cJSON_AddArrayToObject(o, key);

    for (uint8_t i = 0; i < n && arr != NULL; i++)
    {
        cJSON_AddItemToArray(arr, cJSON_CreateString(codes[i]));
    }
}

/* ---- public wrappers (the script_engine binding surface) ------------------ */

esp_err_t autopid_dtc_scan_start(void)
{
    return ap_dtc_scan_start();
}

bool autopid_dtc_scanning(void)
{
    return s_busy;
}

esp_err_t autopid_dtc_report(cJSON **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = ap_dtc_report_json();
    return (*out != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t autopid_dtc_clear(const char *codes, const char *mode,
                            bool *out_cleared)
{
    char err[48];

    return ap_dtc_clear(codes, mode, out_cleared, NULL, NULL, err,
                        sizeof(err));
}

cJSON *ap_dtc_report_json(void)
{
    ap_dtc_report_t r;

    if (ap_dtc_report_get(&r) != ESP_OK)
    {
        return NULL;
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddBoolToObject(o, "valid", r.valid);
    cJSON_AddNumberToObject(o, "ts", (double)r.ts_epoch);
    cJSON_AddBoolToObject(o, "mil", r.mil);
    cJSON_AddNumberToObject(o, "mil_count", r.mil_count);
    cJSON_AddNumberToObject(o, "ecus", r.n_ecus);
    cJSON_AddStringToObject(o, "protocol",
                            r.protocol[0] ? r.protocol : "obd");
    add_code_array(o, "stored", r.stored, r.n_stored);
    add_code_array(o, "pending", r.pending, r.n_pending);
    add_code_array(o, "permanent", r.permanent, r.n_permanent);
    add_code_array(o, "new", r.new_codes, r.n_new);

    /* freeze frame (§14) — absent when none was captured */
    if (r.frz_present)
    {
        cJSON *fz = cJSON_AddObjectToObject(o, "freeze");

        if (fz != NULL)
        {
            cJSON_AddStringToObject(fz, "dtc", r.frz_dtc);

            if (r.frz_ecu != UINT32_MAX)
            {
                char hdr[10];

                snprintf(hdr, sizeof(hdr), "%lX",
                         (unsigned long)r.frz_ecu);
                cJSON_AddStringToObject(fz, "ecu", hdr);
            }

            cJSON *pp = cJSON_AddObjectToObject(fz, "params");

            for (uint8_t i = 0; i < r.n_frz && pp != NULL; i++)
            {
                cJSON *v = cJSON_AddObjectToObject(pp, r.frz[i].name);

                if (v != NULL)
                {
                    cJSON_AddNumberToObject(v, "value",
                                            (double)r.frz[i].value);
                    cJSON_AddStringToObject(v, "unit", r.frz[i].unit);
                }
            }
        }
    }

    if (r.error[0] != '\0')
    {
        cJSON_AddStringToObject(o, "error", r.error);
    }

    return o;
}
