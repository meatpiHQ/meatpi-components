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
 * @file obd_chip_probe.c
 * @brief The wire tools as one runner (issue #1023, 2026-10-28): probe,
 *        scan, the hardware baud detection and the passive listen, each
 *        filling one result the console prints and the HTTP route serves.
 *        The route starts the runner on a one-shot task (a scan takes 13 s,
 *        a detection up to 16 s: never inside an httpd handler) and the page
 *        polls the result.
 */
#include "obd_chip.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

/* the standard bauds: the scan walks them, the detector names the nearest */
static const int BAUD_TABLE[] = { 9600, 19200, 38400, 57600, 115200, 230400,
                                  460800, 500000, 921600, 1000000, 1500000,
                                  2000000, 3000000 };
#define BAUD_TABLE_N (sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]))

/* the job's result: ~3 KB, PSRAM (§2) */
static obd_probe_result_t s_last EXT_RAM_BSS_ATTR;
static volatile bool      s_job_running;

const int *obd_chip_probe_bauds(size_t *count)
{
    if (count != NULL)
    {
        *count = BAUD_TABLE_N;
    }

    return BAUD_TABLE;
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

/** Fill a line's text and hex from the first bytes of @p rx. */
static void line_fill(obd_probe_line_t *ln, int baud, const char *cmd,
                      const uint8_t *rx, size_t stored, uint32_t got,
                      bool prompt)
{
    size_t n = (stored < OBD_PROBE_TEXT) ? stored : OBD_PROBE_TEXT;
    size_t hx = 0;

    memset(ln, 0, sizeof(*ln));
    ln->baud = baud;
    strlcpy(ln->cmd, cmd, sizeof(ln->cmd));
    ln->prompt = prompt;
    ln->bytes = got;

    for (size_t i = 0; i < n; i++)
    {
        ln->text[i] = (rx[i] >= 32 && rx[i] < 127) ? (char)rx[i] : '.';
        hx += (size_t)snprintf(ln->hex + hx, sizeof(ln->hex) - hx, "%s%02X",
                               (i > 0) ? " " : "", rx[i]);
    }

    ln->text[n] = '\0';
}

/** One bare-wire exchange into the next free line. */
static esp_err_t line_probe(obd_probe_result_t *out, int baud,
                            const char *cmd, uint32_t timeout_ms)
{
    uint8_t rx[OBD_PROBE_TEXT];
    size_t got = 0;
    bool prompt = false;
    char line[16];

    if (out->n_lines >= OBD_PROBE_LINES)
    {
        return ESP_ERR_NO_MEM;
    }

    snprintf(line, sizeof(line), "%s\r", cmd);

    esp_err_t err = obd_chip_probe(line, baud, timeout_ms, rx, sizeof(rx),
                                   &got, &prompt);

    if (err != ESP_OK)
    {
        return err;
    }

    line_fill(&out->lines[out->n_lines], baud, cmd, rx,
              (got < sizeof(rx)) ? got : sizeof(rx), (uint32_t)got, prompt);
    out->n_lines++;
    return ESP_OK;
}

/** A passive listen into the raw buffer. */
static esp_err_t raw_listen(obd_probe_result_t *out, int baud, int seconds)
{
    size_t got = 0;
    bool prompt = false;

    esp_err_t err = obd_chip_probe("", baud, (uint32_t)seconds * 1000u,
                                   out->raw, sizeof(out->raw), &got, &prompt);

    if (err != ESP_OK)
    {
        return err;
    }

    out->raw_total = (uint32_t)got;
    out->raw_len = (got < sizeof(out->raw)) ? got : sizeof(out->raw);
    out->raw_baud = (baud > 0) ? baud : obd_uart_get_baud();
    out->raw_prompt = prompt;
    return ESP_OK;
}

/** A stimulus other than ATI is one printable word under OBD_PROBE_CMD
 *  without quotes or backslashes (it lands in JSON as typed). */
static bool stimulus_ok(const char *cmd)
{
    size_t n = strlen(cmd);

    if (n == 0 || n >= OBD_PROBE_CMD)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        if (cmd[i] <= ' ' || cmd[i] > '~' || cmd[i] == '"' || cmd[i] == '\\')
        {
            return false;
        }
    }

    return true;
}

esp_err_t obd_chip_probe_run(obd_probe_kind_t kind, int baud, int seconds,
                             const char *cmd, obd_probe_result_t *out)
{
    if (out == NULL || seconds < 0 || seconds > 60 || baud < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool custom = (cmd != NULL && cmd[0] != '\0');

    if (custom && !stimulus_ok(cmd))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* the stimulus: ATI, or the word asked for (VTDLMIC3422 asks a chip
       whose application is gone for its bootloader, 2026-10-28) */
    const char *stim = custom ? cmd : "ATI";

    memset(out, 0, sizeof(*out));
    out->running = true;
    out->kind = kind;
    out->baud = baud;
    out->seconds = seconds;
    out->at_us = esp_timer_get_time();

    if (custom)
    {
        strlcpy(out->cmd, cmd, sizeof(out->cmd));
    }

    esp_err_t err = ESP_OK;

    switch (kind)
    {
        case OBD_PROBE_PROBE:
        {
            const int both[2] = { OBD_CHIP_BAUD, OBD_CHIP_FALLBACK_BAUD };
            const int *bauds = (baud > 0) ? &baud : both;
            size_t n = (baud > 0) ? 1 : 2;

            for (size_t b = 0; b < n && err == ESP_OK; b++)
            {
                err = line_probe(out, bauds[b], stim, 1500);

                if (err == ESP_OK && !custom)
                {
                    err = line_probe(out, bauds[b], "VTVERS", 1500);
                }
            }

            break;
        }

        case OBD_PROBE_SCAN:
            for (size_t b = 0; b < BAUD_TABLE_N && err == ESP_OK; b++)
            {
                uint8_t rx[4];
                size_t got = 0;
                bool prompt = false;

                /* a lone CR first: a chip mid-line answers the next clean */
                (void)obd_chip_probe("\r", BAUD_TABLE[b], 150, rx, sizeof(rx),
                                     &got, &prompt);
                err = line_probe(out, BAUD_TABLE[b], stim, 600);

                if (err == ESP_OK && out->lines[out->n_lines - 1].prompt &&
                    out->detected == 0)
                {
                    out->detected = BAUD_TABLE[b]; /* the first prompt */
                }
            }

            break;

        case OBD_PROBE_AUTO:
        {
            obd_chip_autobaud_t ab;
            char stimulus[OBD_PROBE_CMD + 2];
            int was = obd_uart_get_baud();

            /* the stimulus goes out at the UART's baud, or the one asked */
            snprintf(stimulus, sizeof(stimulus), "\r%s\r", stim);

            if (baud > 0 && baud != was)
            {
                (void)obd_uart_set_baud(baud);
            }

            err = obd_chip_autobaud(stimulus,
                                    (uint32_t)((seconds > 0) ? seconds : 10) *
                                        1000u, &ab);

            if (baud > 0 && baud != was)
            {
                (void)obd_uart_set_baud(was);
            }

            if (err == ESP_OK)
            {
                out->edges = ab.edges;
                out->measured = (ab.edges > 0) ? ab.baud_lh : 0;
                out->detected = nearest_standard_baud(out->measured);

                if (out->detected != 0)
                {
                    err = raw_listen(out, out->detected, 6);
                }
            }

            break;
        }

        case OBD_PROBE_LISTEN:
            err = raw_listen(out, baud, (seconds > 0) ? seconds : 5);
            break;

        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    out->err = err;
    out->done = true;
    out->running = false;
    return err;
}

/* ---- the job (HTTP) ---------------------------------------------------------- */

typedef struct
{
    obd_probe_kind_t kind;
    int              baud;
    int              seconds;
    char             cmd[OBD_PROBE_CMD];
} probe_job_t;

static probe_job_t s_job;

static void probe_job_task(void *arg)
{
    (void)arg;

    esp_err_t err = obd_chip_probe_run(s_job.kind, s_job.baud, s_job.seconds,
                                       s_job.cmd, &s_last);

    ESP_LOGI(TAG, "wire tool %d done (%s), stack_hw=%u B", (int)s_job.kind,
             esp_err_to_name(err), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_job_running = false;
    vTaskDelete(NULL);
}

esp_err_t obd_chip_probe_start(obd_probe_kind_t kind, int baud, int seconds,
                               const char *cmd)
{
    if (kind > OBD_PROBE_LISTEN || seconds < 0 || seconds > 60 || baud < 0 ||
        (cmd != NULL && cmd[0] != '\0' && !stimulus_ok(cmd)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_job_running || obd_chip_update_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_job.kind = kind;
    s_job.baud = baud;
    s_job.seconds = seconds;
    strlcpy(s_job.cmd, (cmd != NULL) ? cmd : "", sizeof(s_job.cmd));

    /* the page sees "running" from the first poll on */
    memset(&s_last, 0, sizeof(s_last));
    s_last.running = true;
    s_last.kind = kind;
    s_last.baud = baud;
    s_last.seconds = seconds;
    s_job_running = true;

    /* internal stack: UART driver path (the bring-up task's rule) */
    if (xTaskCreate(probe_job_task, "obd_probe", 4096, NULL, 5, NULL) != pdPASS)
    {
        s_job_running = false;
        s_last.running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void obd_chip_probe_last(obd_probe_result_t *out)
{
    if (out != NULL)
    {
        *out = s_last;
    }
}
