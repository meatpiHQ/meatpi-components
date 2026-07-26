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
 * @file autopid_std.c
 * @brief Standard (SAE mode-01) PIDs: the vendored legacy table as data,
 *        the PURE bit_start->expression mapping + scan-response parser
 *        (host-tested), and the target-only async support scan
 *        ("scan once, store" — TASK_autopid.md §7).
 *
 * Scan flow: POST /api/autopid/std_scan spawns a short-lived task
 * (INTERNAL stack — it writes /data at the end, §2) that pauses the
 * poller, sets the configured protocol, walks the 0100/0120/../01A0
 * support bitmaps (multi-ECU responses OR-merged), maps bits to table
 * entries and stores /data/autopid/std_scan.json atomically. No
 * automatic rescan ever — the UI owns the button.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AUTOPID_HOST_TEST
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "filesystem.h"
#include "obd_chip.h"
#endif

#include "obd2_standard_pids.h"

/* ---- PURE: legacy table row -> v6 expression ---------------------------------- */

/** Print @p v as a parser-safe decimal literal: %.10g normally, but the
 *  grammar has no exponent form — tiny scales (1/32768) fall back to
 *  fixed-point with trailing zeros trimmed. */
static void fmt_literal(double v, char *out, size_t len)
{
    snprintf(out, len, "%.10g", v);

    if (strchr(out, 'e') != NULL)
    {
        snprintf(out, len, "%.15f", v);

        char *dot = strchr(out, '.');

        if (dot != NULL)
        {
            char *end = out + strlen(out) - 1;

            while (end > dot + 1 && *end == '0')
            {
                *end-- = '\0';
            }
        }
    }
}

esp_err_t ap_std_expression(uint8_t bit_start, uint8_t bit_length,
                            double scale, double offset, char *buf,
                            size_t buf_len)
{
    if (buf == NULL || buf_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* placeholder rows (bit_start 0 pairs with bit_length 0) */
    if (bit_length == 0 || bit_start < 8)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    int first = bit_start / 8 - 1;              /* drop legacy's PCI byte */
    int count = (bit_length + 7) / 8;
    char base[24];

    if (count == 1)
    {
        snprintf(base, sizeof(base), "B%d", first);
    }
    else
    {
        snprintf(base, sizeof(base), "[B%d:B%d]", first, first + count - 1);
    }

    /* value = raw * scale + offset */
    char sc[32], of[32];

    fmt_literal(scale, sc, sizeof(sc));
    fmt_literal(fabs(offset), of, sizeof(of));

    int n;

    if (scale == 1.0 && offset == 0.0)
    {
        n = snprintf(buf, buf_len, "%s", base);
    }
    else if (offset == 0.0)
    {
        n = snprintf(buf, buf_len, "%s*%s", base, sc);
    }
    else if (scale == 1.0)
    {
        n = snprintf(buf, buf_len, "%s%c%s", base,
                     (offset < 0) ? '-' : '+', of);
    }
    else
    {
        n = snprintf(buf, buf_len, "%s*%s%c%s", base, sc,
                     (offset < 0) ? '-' : '+', of);
    }

    return (n > 0 && (size_t)n < buf_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ---- PURE: support-bitmap response parser -------------------------------------- */

/** Parse one line's hex byte tokens into @p bytes; -1 on non-hex noise. */
static int line_bytes(const char *line, size_t len, uint8_t *bytes,
                      size_t max)
{
    size_t n = 0, pos = 0;

    while (pos < len && n < max)
    {
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        {
            pos++;
        }

        size_t start = pos;

        while (pos < len && line[pos] != ' ' && line[pos] != '\t')
        {
            pos++;
        }

        size_t tlen = pos - start;

        if (tlen == 0)
        {
            continue;
        }

        /* accept 2-hex byte tokens and 3-hex CAN headers (skipped by
           the 41-anchor below); anything else = not a data line */
        if (tlen > 3)
        {
            return -1;
        }

        for (size_t i = 0; i < tlen; i++)
        {
            if (!isxdigit((unsigned char)line[start + i]))
            {
                return -1;
            }
        }

        if (tlen <= 2)
        {
            char tok[3] = { line[start], (tlen == 2) ? line[start + 1]
                                                     : '\0', '\0' };

            bytes[n++] = (uint8_t)strtol(tok, NULL, 16);
        }
        /* 3-hex header token: ignore (the anchor search skips it) */
    }

    return (int)n;
}

bool ap_std_scan_parse(const char *resp, uint8_t expect_pid,
                       uint32_t *bitmap)
{
    if (resp == NULL || bitmap == NULL)
    {
        return false;
    }

    *bitmap = 0;

    bool found = false;
    const char *p = resp;

    while (*p != '\0')
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n')
        {
            eol++;
        }

        uint8_t bytes[16];
        int n = line_bytes(p, (size_t)(eol - p), bytes,
                           sizeof(bytes));

        /* anchor on "41 <pid>" anywhere in the line — headers-on rows
           (11-bit, 3-hex header + PCI) work because the anchor skips
           leading bytes. Noise lines (SEARCHING, "N:" ISO rows, 8-hex
           29-bit headers) fail tokenization and are skipped; the scan
           prelude pins ATH0 so bitmap rows are plain "41 XX ..". */
        for (int i = 0; n > 0 && i + 1 < n; i++)
        {
            if (bytes[i] == 0x41 && bytes[i + 1] == expect_pid)
            {
                uint32_t bm = 0;
                int have = 0;

                for (int j = 0; j < 4 && i + 2 + j < n; j++)
                {
                    bm = (bm << 8) | bytes[i + 2 + j];
                    have++;
                }

                if (have == 4)
                {
                    *bitmap |= bm;      /* multi-ECU rows OR together */
                    found = true;
                }

                break;
            }
        }

        p = eol;

        while (*p == '\r' || *p == '\n')
        {
            p++;
        }
    }

    return found;
}

/* ---- PURE: mode-02 freeze-frame decode (TASK_dtc §14) --------------------------- */

int ap_frz_decode(const uint8_t *payload, size_t len, ap_frz_val_t *out,
                  int n, int max)
{
    /* [0x42, pid, frame, A, B, …] — same byte semantics as the mode-01
       expressions, shifted by the extra frame byte */
    if (payload == NULL || out == NULL || len < 4 || payload[0] != 0x42)
    {
        return n;
    }

    const std_pid_t *info = get_pid(payload[1]);

    if (info == NULL || info->base_name == NULL)
    {
        return n;
    }

    for (int i = 0; i < info->num_params && n < max; i++)
    {
        const std_parameter_t *prm = &info->params[i];

        /* the ap_std_expression support rule: placeholders + sub-PCI
           rows are not decodable */
        if (prm->name == NULL || prm->bit_length == 0 || prm->bit_start < 8)
        {
            continue;
        }

        /* mode-01 payload index (B0 = service echo), then +1 for the
           mode-02 frame byte */
        int first = prm->bit_start / 8 - 1;
        int count = (prm->bit_length + 7) / 8;

        if ((size_t)(first + 1 + count) > len)
        {
            continue;   /* short payload — skip, keep what fits */
        }

        uint32_t raw = 0;

        for (int b = 0; b < count; b++)
        {
            raw = (raw << 8) | payload[first + 1 + b];
        }

        snprintf(out[n].name, sizeof(out[n].name), "%s", prm->name);
        snprintf(out[n].unit, sizeof(out[n].unit), "%s",
                 (prm->unit != NULL) ? prm->unit : "");
        out[n].value = (float)raw * prm->scale + prm->offset;
        n++;
    }

    return n;
}

/* ---- target half: table JSON + the async scan job ------------------------------ */

#ifndef AUTOPID_HOST_TEST

static const char *TAG = "autopid";

#define AP_STD_SCAN_PATH "/data/autopid/std_scan.json"
#define AP_SCAN_REQ_TIMEOUT pdMS_TO_TICKS(10000) /* SEARCHING can be slow */

typedef enum
{
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_DONE,
    SCAN_FAILED,
} scan_state_t;

static volatile scan_state_t s_scan_state;
static uint16_t s_scan_found;
static char     s_scan_err[64];
static int64_t  s_scan_ts;      /* epoch seconds of last completed scan */

static StaticTask_t s_scan_tcb;                    /* internal object    */
static StackType_t  s_scan_stack[6144];            /* INTERNAL: fs write */

const char *autopid_std_scan_path(void)
{
    return AP_STD_SCAN_PATH;
}

/** Append one table entry (with generated expressions) to @p arr. */
static bool std_entry_to_json(cJSON *arr, uint8_t pid)
{
    const std_pid_t *info = get_pid(pid);

    if (info == NULL || info->base_name == NULL)
    {
        return false;
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return false;
    }

    char cmd[8];

    snprintf(cmd, sizeof(cmd), "01%02X", pid);
    cJSON_AddNumberToObject(o, "pid", pid);
    cJSON_AddStringToObject(o, "cmd", cmd);
    cJSON_AddStringToObject(o, "name", info->base_name);

    cJSON *params = cJSON_AddArrayToObject(o, "parameters");

    for (int i = 0; i < info->num_params; i++)
    {
        const std_parameter_t *prm = &info->params[i];
        char expr[AP_EXPR_LEN];

        if (prm->name == NULL ||
            ap_std_expression(prm->bit_start, prm->bit_length,
                              (double)prm->scale, (double)prm->offset,
                              expr, sizeof(expr)) != ESP_OK)
        {
            continue;   /* placeholder row */
        }

        cJSON *pj = cJSON_CreateObject();

        if (pj == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(pj, "name", prm->name);
        cJSON_AddStringToObject(pj, "expression", expr);
        cJSON_AddStringToObject(pj, "unit", prm->unit ? prm->unit : "");
        cJSON_AddStringToObject(pj, "class",
                                prm->class ? prm->class : "");

        if (!(prm->min == 0.0f && prm->max == 0.0f))
        {
            cJSON_AddNumberToObject(pj, "min", prm->min);
            cJSON_AddNumberToObject(pj, "max", prm->max);
        }

        cJSON_AddItemToArray(params, pj);
    }

    cJSON_AddItemToArray(arr, o);
    return true;
}

cJSON *ap_std_table_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    if (arr == NULL)
    {
        return NULL;
    }

    for (int pid = 1; pid < 256; pid++)
    {
        (void)std_entry_to_json(arr, (uint8_t)pid);
    }

    return arr;
}

/** The protocol prelude, legacy map: ATTP + header/mask for 6..9. */
static const char *scan_prelude(void)
{
    const char *proto = ap_core_std_protocol();

    switch (proto[0] != '\0' ? proto[0] : '0')
    {
        case '6': return "ATS1;ATH0;ATST96;ATTP6;ATSH7DF;ATCRA";
        case '7': return "ATS1;ATH0;ATST96;ATTP7;ATSH18DB33F1;ATCRA";
        case '8': return "ATS1;ATH0;ATST96;ATTP8;ATSH7DF;ATCRA";
        case '9': return "ATS1;ATH0;ATST96;ATTP9;ATSH18DB33F1;ATCRA";
        default:  return "ATS1;ATH0;ATST96;ATTP0";
    }
}

static void scan_task(void *arg)
{
    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* scan task only */

    ap_core_scan_pause(true);

    /* prelude: one command at a time (';'-separated) */
    {
        const char *p = scan_prelude();

        while (*p != '\0')
        {
            const char *sep = strchr(p, ';');
            size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
            char one[24];

            if (len > 0 && len < sizeof(one))
            {
                memcpy(one, p, len);
                one[len] = '\0';
                (void)obd_chip_request(one, resp, sizeof(resp),
                                       pdMS_TO_TICKS(2000));
            }

            p += len + ((sep != NULL) ? 1 : 0);
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *supported = NULL;
    uint16_t found = 0;
    bool any_response = false;

    if (root != NULL)
    {
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddStringToObject(root, "protocol", ap_core_std_protocol());
        supported = cJSON_AddArrayToObject(root, "supported");
    }

    for (int range = 0; range < 6 && supported != NULL; range++)
    {
        uint8_t base = (uint8_t)(range * 0x20);
        char cmd[8];

        snprintf(cmd, sizeof(cmd), "01%02X", base);

        if (obd_chip_request(cmd, resp, sizeof(resp),
                             AP_SCAN_REQ_TIMEOUT) != ESP_OK)
        {
            break;
        }

        uint32_t bitmap = 0;

        if (!ap_std_scan_parse(resp, base, &bitmap))
        {
            break;      /* NO DATA / noise — range unsupported */
        }

        any_response = true;

        for (int bit = 0; bit < 31; bit++)  /* bit 31 = next-range flag */
        {
            if (bitmap & (1u << (31 - bit)))
            {
                uint8_t pid = (uint8_t)(base + bit + 1);

                if (std_entry_to_json(supported, pid))
                {
                    found++;
                }
            }
        }

        if ((bitmap & 1u) == 0)
        {
            break;      /* next range not supported */
        }
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
    s_scan_state = (err == ESP_OK) ? SCAN_DONE : SCAN_FAILED;
    ap_core_job_release();
    ap_events_scan_done(found);
    ESP_LOGI(TAG, "std scan %s: %u PIDs",
             (err == ESP_OK) ? "done" : "FAILED", found);

    /* ephemeral tasks escape System Monitor — surface the watermark
       for the stack-audit bench (same net as the dtc job task) */
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
    s_scan_found = 0;
    s_scan_err[0] = '\0';

    /* INTERNAL stack: the job ends in a /data write (§2) */
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

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(o, "status", NAMES[s_scan_state]);
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

#endif /* !AUTOPID_HOST_TEST */
