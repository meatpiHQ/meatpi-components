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
 * @file can_selftest.c
 * @brief The engine: one `010C` from the chip, answered by the ESP32 as an
 *        engine ECU from 7EF, on the CAN pair both share.
 *
 * Sequence (about 2 s):
 *   1. preconditions: the CAN bus running, the chip up and idle (no app on
 *      it), the bus readable (silent or live, both run; unreadable refuses);
 *      a listen-only node on a silent `auto` bus adopts the bitrate first
 *   2. the ESP32 subscribes to 7DF and reads the TWAI counters
 *   3. the chip, in one transaction: ATTPn (the bus bitrate), ATH1, ATSH 7DF,
 *      then `010C` written raw while this task plays the ECU: the 7DF frame
 *      [02 01 0C] arrives on the queue, the reply 7EF [04 41 0C 1A F8] goes
 *      out, the chip's output is collected to its prompt
 *   4. restore: ATPC, ATD; the queue unsubscribed; AutoPID resumes
 *   5. the verdict from what each side saw (can_selftest_parse.c)
 */
#include "can_selftest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_manager.h"
#include "log_manager.h"
#include "obd_chip.h"

static const char *TAG = "can_selftest";

#define CST_REQ_ID      0x7DFu /* the functional request id every app uses */
#define CST_REPLY_ID    0x7EFu /* ECU number 8's response id: no car uses it */
#define CST_WAIT_REQ_MS 400    /* the chip's request on the ESP32's queue    */
#define CST_WAIT_OUT_MS 900    /* the chip's output to its prompt            */
#define CST_RPM_A       0x1A   /* 1726 rpm: (0x1A * 256 + 0xF8) / 4          */
#define CST_RPM_B       0xF8

/* the job's result (~1.5 KB) and the exchange's chip output: PSRAM (§2) */
static can_selftest_result_t s_last EXT_RAM_BSS_ATTR;
static volatile bool s_running;

static const char *const STEP_NAMES[CAN_SELFTEST_STEPS] =
{
    "The chip asked, the ESP32 heard",
    "The ESP32 answered, the chip read it",
    "Bus health and clean-up",
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void line(can_selftest_result_t *r, const char *fmt, ...)
{
    if (r->n_lines >= CAN_SELFTEST_LINES)
    {
        return;
    }

    va_list ap;

    va_start(ap, fmt);
    vsnprintf(r->lines[r->n_lines], CAN_SELFTEST_LINE, fmt, ap);
    va_end(ap);
    r->n_lines++;
}

static void refuse(can_selftest_result_t *r, const char *why)
{
    strlcpy(r->refusal, why, sizeof(r->refusal));
    r->verdict = r->refusal;
    r->pass = false;
    r->err = ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "not run: %s", why);
}

/** One chip command inside the transaction; the answer trimmed to one line
 *  for the exchange log. false on a timeout or an error text. */
static bool chip_cmd(can_selftest_result_t *r, const char *cmd)
{
    char resp[96];
    esp_err_t err = obd_chip_request(cmd, resp, sizeof(resp),
                                     pdMS_TO_TICKS(1500));

    for (char *p = resp; err == ESP_OK && *p != '\0'; p++)
    {
        if (*p == '\r' || *p == '\n')
        {
            *p = ' ';
        }
    }

    line(r, "chip   %-9s %s", cmd, (err == ESP_OK) ? resp : esp_err_to_name(err));
    return err == ESP_OK && strchr(resp, '?') == NULL;
}

/** Collect the chip's output (through the fan-out) until a prompt at a line
 *  start or the deadline; returns the text without the prompt. */
static size_t collect_output(QueueHandle_t q, char *out, size_t out_len,
                             uint32_t budget_ms)
{
    size_t n = 0;
    char prev = '\n';
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(budget_ms);

    while (xTaskGetTickCount() < deadline)
    {
        obd_chunk_t ch;
        TickType_t left = deadline - xTaskGetTickCount();

        if (xQueueReceive(q, &ch, left) != pdTRUE)
        {
            break;
        }

        for (size_t i = 0; i < ch.len; i++)
        {
            char c = (char)ch.data[i];

            if (c == '>' && (prev == '\r' || prev == '\n'))
            {
                out[n] = '\0';
                return n;
            }

            if (n + 1 < out_len)
            {
                out[n++] = c;
            }

            prev = c;
        }
    }

    out[n] = '\0';
    return n;
}

static void run(can_selftest_result_t *r)
{
    static char chip_out[512] EXT_RAM_BSS_ATTR;
    uint32_t t0 = now_ms();

    memset(r, 0, sizeof(*r));
    r->running = true;
    r->at_us = esp_timer_get_time();
    r->rpm = -1;

    for (size_t i = 0; i < CAN_SELFTEST_STEPS; i++)
    {
        r->steps[i].name = STEP_NAMES[i];
    }

    /* 1. preconditions */
    can_manager_status_t cs;

    if (can_manager_status(&cs) != ESP_OK || !cs.running)
    {
        refuse(r, "the CAN bus is off: enable it in CAN bus settings");
        goto out;
    }

    if (!obd_chip_ready())
    {
        refuse(r, "the OBD chip is not up: see the chip's wire tools");
        goto out;
    }

    if (obd_chip_client_idle_ms() < 10000)
    {
        refuse(r, "an app is using the OBD chip; try again when it is idle");
        goto out;
    }

    can_manager_probe_t pr;

    if (can_manager_probe(&pr) != ESP_OK)
    {
        refuse(r, "the bus could not be listened to");
        goto out;
    }

    r->live = (pr.result == CAN_PROBE_LIVE);

    if (pr.result == CAN_PROBE_UNREADABLE)
    {
        refuse(r, "the bus talks at a bitrate this node cannot read");
        goto out;
    }

    /* a listen-only node on a silent bus (baud = auto, nothing proven yet)
       may state the bitrate itself: the configured one, 500 when auto */
    if (cs.link.listen_only && !r->live)
    {
        uint32_t want = (cs.baud_kbps != 0) ? cs.baud_kbps : 500;

        (void)can_manager_adopt_baud(want);
        (void)can_manager_status(&cs);
    }

    if (!can_manager_tx_ready())
    {
        refuse(r, "the CAN node may not transmit yet (bitrate not proven)");
        goto out;
    }

    r->baud_kbps = cs.link.baud_kbps ? cs.link.baud_kbps : cs.baud_kbps;
    line(r, "can    listened %lu ms at %lu kbit/s: %s", (unsigned long)pr.took_ms,
         (unsigned long)r->baud_kbps, r->live ? "live" : "silent");

    const char *proto = (r->baud_kbps == 500) ? "ATTP6"
                        : (r->baud_kbps == 250) ? "ATTP8" : NULL;

    if (proto == NULL)
    {
        refuse(r, "the chip has no 11-bit protocol for this bitrate (500 or 250 only)");
        goto out;
    }

    /* 2. the ESP32 side: the request's queue and the counters before */
    QueueHandle_t q = xQueueCreate(8, sizeof(can_core_frame_t));
    int sub = -1;

    if (q == NULL || can_manager_subscribe_queue(q, CST_REQ_ID, 0x7FF, false,
                                                 false, &sub) != ESP_OK)
    {
        refuse(r, "no CAN subscription slot free");

        if (q != NULL)
        {
            vQueueDelete(q);
        }

        goto out;
    }

    (void)can_manager_subscriber_name(sub, "can_selftest");
    can_core_stats_t before = cs.stats;

    /* 3. the chip side, one transaction; AutoPID yields as for an app */
    QueueHandle_t cq = xQueueCreate(32, sizeof(obd_chunk_t));
    esp_err_t err = (cq != NULL) ? obd_chip_txn_begin(pdMS_TO_TICKS(3000))
                                 : ESP_ERR_NO_MEM;

    if (err != ESP_OK)
    {
        refuse(r, (err == ESP_ERR_INVALID_STATE)
                      ? "the OBD chip is busy (a monitor or an update)"
                      : "the OBD chip could not be claimed");
        can_manager_unsubscribe_queue(sub);
        vQueueDelete(q);

        if (cq != NULL)
        {
            vQueueDelete(cq);
        }

        goto out;
    }

    /* no obd_chip_client_touch() here: the transaction's claim already keeps
       AutoPID's requests out for the 2 s, and the mark would make the next
       run refuse itself as "an app is using the chip" */
    uint32_t t1 = now_ms();
    bool prepped = chip_cmd(r, proto) && chip_cmd(r, "ATH1") &&
                   chip_cmd(r, "ATSH 7DF") && chip_cmd(r, "ATCAF1");

    if (!prepped)
    {
        line(r, "chip   the preparation was refused; the test goes on");
    }

    /* the request, written raw so this task is free to play the ECU */
    obd_chip_subscribe(cq, "can_selftest");
    obd_chip_send((const uint8_t *)"010C\r", 5);
    line(r, "chip   010C  (sent)");

    can_core_frame_t f;
    uint32_t t_req = now_ms();

    r->esp_heard = false;

    while (xQueueReceive(q, &f, pdMS_TO_TICKS(CST_WAIT_REQ_MS)) == pdTRUE)
    {
        if (f.id == CST_REQ_ID && f.dlc >= 3 && f.data[0] == 0x02 &&
            f.data[1] == 0x01 && f.data[2] == 0x0C)
        {
            r->esp_heard = true;
            line(r, "can    rx 7DF [%02X %02X %02X] after %lu ms", f.data[0],
                 f.data[1], f.data[2], (unsigned long)(now_ms() - t_req));

            uint8_t reply[8] = { 0x04, 0x41, 0x0C, CST_RPM_A, CST_RPM_B, 0, 0, 0 };
            esp_err_t tx = can_manager_send(CST_REPLY_ID, false, false, reply, 8);

            line(r, "can    tx 7EF [04 41 0C %02X %02X]: %s", CST_RPM_A, CST_RPM_B,
                 (tx == ESP_OK) ? "queued" : esp_err_to_name(tx));
            break;
        }
    }

    r->steps[0].ok = r->esp_heard;
    r->steps[0].ms = now_ms() - t_req;
    snprintf(r->steps[0].detail, sizeof(r->steps[0].detail),
             r->esp_heard ? "the chip sent 7DF 02 01 0C at %lu kbit/s; the "
                            "ESP32 received it and acknowledged it"
                          : "no 7DF frame reached the ESP32 within %lu ms",
             r->esp_heard ? (unsigned long)r->baud_kbps
                          : (unsigned long)CST_WAIT_REQ_MS);

    /* what the chip printed, to its prompt */
    uint32_t t_out = now_ms();
    size_t got = collect_output(cq, chip_out, sizeof(chip_out), CST_WAIT_OUT_MS);

    obd_chip_unsubscribe(cq);
    r->reply = can_selftest_parse_reply(chip_out, CST_REPLY_ID, &r->rpm);

    for (char *p = chip_out; *p != '\0'; p++)
    {
        if (*p == '\r' || *p == '\n')
        {
            *p = ' ';
        }
    }

    line(r, "chip   printed: %s", (got > 0) ? chip_out : "(nothing)");
    r->steps[1].ok = (r->reply == CAN_SELFTEST_REPLY_OURS);
    r->steps[1].ms = now_ms() - t_out;

    switch (r->reply)
    {
        case CAN_SELFTEST_REPLY_OURS:
            snprintf(r->steps[1].detail, sizeof(r->steps[1].detail),
                     "the ESP32 sent 7EF 04 41 0C %02X %02X; the chip printed "
                     "it: engine speed %d rpm, from the ESP32", CST_RPM_A,
                     CST_RPM_B, r->rpm);
            break;
        case CAN_SELFTEST_REPLY_NODATA:
            strlcpy(r->steps[1].detail, "the chip printed NO DATA: the ESP32's reply never reached it", sizeof(r->steps[1].detail));
            break;
        case CAN_SELFTEST_REPLY_OTHER:
            strlcpy(r->steps[1].detail, "other ECUs answered the chip, the ESP32's reply did not reach it", sizeof(r->steps[1].detail));
            break;
        case CAN_SELFTEST_REPLY_CANERROR:
            strlcpy(r->steps[1].detail, "the chip printed CAN ERROR: nothing on the bus acknowledged its request", sizeof(r->steps[1].detail));
            break;
        default:
            strlcpy(r->steps[1].detail, "the chip printed nothing usable in time", sizeof(r->steps[1].detail));
            break;
    }

    /* 4. restore */
    uint32_t t_rest = now_ms();
    bool restored = chip_cmd(r, "ATPC") && chip_cmd(r, "ATD");

    obd_chip_txn_end();
    vQueueDelete(cq);
    can_manager_unsubscribe_queue(sub);
    vQueueDelete(q);

    can_manager_status_t after;
    bool bus_ok = (can_manager_status(&after) == ESP_OK);
    uint32_t bus_err = bus_ok ? (after.stats.bus_errors - before.bus_errors) : 0;
    uint32_t tx_err = bus_ok ? (after.stats.tx_errors - before.tx_errors) : 0;

    r->steps[2].ok = restored && bus_err == 0 && tx_err == 0;
    r->steps[2].ms = now_ms() - t_rest;
    (void)t1;
    snprintf(r->steps[2].detail, sizeof(r->steps[2].detail),
             "%lu bus error(s), %lu tx error(s); chip restored%s, AutoPID "
             "resumes",
             (unsigned long)bus_err, (unsigned long)tx_err,
             restored ? "" : " (the restore was refused)");

    /* 5. the verdict */
    r->verdict = can_selftest_verdict(r->esp_heard, r->reply, &r->pass);
    r->err = ESP_OK;

out:
    r->took_ms = now_ms() - t0;
    r->done = true;
    r->running = false;

    if (r->refusal[0] == '\0')
    {
        ESP_LOGI(TAG, "%s (%lu ms)", r->verdict, (unsigned long)r->took_ms);
    }
}

esp_err_t can_selftest_run(can_selftest_result_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;
    run(out);
    s_last = *out;
    s_running = false;
    return out->err;
}

static void job_task(void *arg)
{
    (void)arg;
    run(&s_last);
    ESP_LOGI(TAG, "job stack_hw=%u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t can_selftest_run_start(void)
{
    if (s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;
    memset(&s_last, 0, sizeof(s_last));
    s_last.running = true;
    s_last.rpm = -1;

    /* internal stack: the UART and TWAI driver paths (the bring-up's rule) */
    if (xTaskCreate(job_task, "can_selftest", 4096, NULL, 5, NULL) != pdPASS)
    {
        s_running = false;
        s_last.running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void can_selftest_last(can_selftest_result_t *out)
{
    if (out != NULL)
    {
        *out = s_last;
    }
}

/* ---- lifecycle (§3) ---------------------------------------------------------- */

esp_err_t can_selftest_settings_register(void); /* can_selftest_settings.c */

esp_err_t can_selftest_init(void)
{
    static const log_descriptor_t LOG_DESC = { "can_selftest", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return can_selftest_settings_register();
}

esp_err_t can_selftest_start(void)
{
    return ESP_OK; /* nothing runs until a test is asked for */
}

esp_err_t can_selftest_stop(void)
{
    return ESP_OK;
}
