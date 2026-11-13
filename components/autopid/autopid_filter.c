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
 * @file autopid_filter.c
 * @brief The ATMA filter window (Phase 4): capture ONE frame with the
 *        filter's CAN id and evaluate every enabled parameter from it.
 *
 * Flow per window, for a scheduled filter entry (poller-task context) and
 * for the test of one (POST /api/autopid/test "filter", 2026-11-14:
 * httpd-task context behind the job lock with the poller parked, nothing
 * published):
 *   ATTP<p>    (only when the id's width is not the protocol's: the
 *              sibling at the same bit rate, see flt_proto_for)
 *   ATCRA<id>  (COMMAND transaction: hardware-filters the monitor)
 *   ATCAF0     (the frame as it is on the bus, not read as a diagnostic
 *              message: see flt_window)
 *   claim MONITOR -> subscribe -> "ATMA" -> collect lines until a
 *   matching frame or monitor_ms elapses -> SPACE stop -> drain to the
 *   '>' prompt -> release -> unsubscribe -> ATCAF1, ATCRA, ATTP<the
 *   chip's own> -> ap_runner_reset() (the window changed ATCRA/monitor
 *   state under the PID runner).
 * The two middle steps and the protocol are the legacy firmware's (v4.50
 * `send_can_filter_cmd`, `ATCAF0` / `ATCAF1` around `ATMA`); the rewrite
 * for v6 had lost them until 2026-11-14.
 *
 * While the window is open, other chip masters' ap_be()->request()
 * fails fast with ESP_ERR_INVALID_STATE (v1 "manual" arbitration):
 * windows should be short (default 1000 ms) and scheduled sparsely.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "battery_monitor.h"
#include "obd_chip.h"

#include "autopid_transport.h"

#include "expression_parser.h"

#include "autopid_bus.h"
#include "autopid_chain_private.h"
#include "autopid_private.h"
#include "autopid_runner_private.h"

static const char *TAG = "autopid";

#define AP_FLT_SETUP_TIMEOUT  pdMS_TO_TICKS(2000)
#define AP_FLT_CLAIM_TIMEOUT  pdMS_TO_TICKS(2000)
#define AP_FLT_DRAIN_MS       1000   /* post-stop prompt wait            */
#define AP_FLT_Q_LEN          16

static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;                       /* internal object   */
static uint8_t s_q_store[AP_FLT_Q_LEN * sizeof(obd_chunk_t)]
    EXT_RAM_BSS_ATTR;

/* per-parameter captured frame: a multiplexed message carries each
   parameter's data in a DIFFERENT frame (mux page), so the window
   collects one frame per parameter instead of one per filter */
typedef struct
{
    uint8_t data[AP_PAYLOAD_MAX];
    size_t  len;
    bool    have;
} flt_slot_t;

/* one window at a time: the poller's, or a test's behind the job lock
   with the poller parked */
static flt_slot_t s_slot[AP_PARAMS_PER] EXT_RAM_BSS_ATTR;
static ap_flt_want_t s_want[AP_PARAMS_PER] EXT_RAM_BSS_ATTR; /* poller task */

/** Stop the monitor and drain until the prompt. A lingering monitor
 *  poisons every later transaction (any byte terminates it and the
 *  "STOPPED" lands in someone else's response window), so retry the
 *  stop once before giving up. */
static void window_close(void)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (ap_be()->monitor_stop() != ESP_OK)
        {
            ESP_LOGW(TAG, "filter: monitor stop send failed");
        }

        int64_t deadline = esp_timer_get_time() + AP_FLT_DRAIN_MS * 1000;
        obd_chunk_t c;

        while (esp_timer_get_time() < deadline)
        {
            if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(50)) != pdTRUE)
            {
                continue;
            }

            for (uint16_t i = 0; i < c.len; i++)
            {
                if (c.data[i] == '>')
                {
                    return;
                }
            }
        }
    }

    ESP_LOGW(TAG, "filter: no prompt after monitor stop (x2)");
}

/** Hand a captured frame to every slot that still waits for it.
 *  @return the slots it filled. */
static uint16_t flt_take(const ap_flt_want_t *want, uint16_t n,
                         const uint8_t *payload, size_t len, double volts)
{
    uint16_t took = 0;

    for (uint16_t i = 0; i < n; i++)
    {
        if (!want[i].want || s_slot[i].have)
        {
            continue;
        }

        if (want[i].mux_expr != NULL && want[i].mux_expr[0] != '\0')
        {
            double mv = 0;

            if (expression_parser_eval(want[i].mux_expr, payload, len, volts,
                                       &mv) != ESP_OK ||
                fabs(mv - (double)want[i].mux_val) > 0.5)
            {
                continue;       /* different mux page                  */
            }
        }

        memcpy(s_slot[i].data, payload, len);
        s_slot[i].len = len;
        s_slot[i].have = true;
        took++;
    }

    return took;
}

/** The window's chip settings back: CAN auto formatting on, receive-all,
 *  and the protocol the chip was on when the window took its sibling
 *  (@p proto_back, '\0' = it did not). When the chip does not take one
 *  (another master holds it), the baseline restore before the next row
 *  type does (mem->moved). */
static void flt_settings_back(ap_tr_t *tr, char proto_back)
{
    char resp[64];
    esp_err_t caf = ap_base_request_tr("ATCAF1", resp, sizeof(resp),
                                       AP_FLT_SETUP_TIMEOUT, tr);
    esp_err_t cra = ap_base_request_tr("ATCRA", resp, sizeof(resp),
                                       AP_FLT_SETUP_TIMEOUT, tr);

    if (caf != ESP_OK || cra != ESP_OK)
    {
        ap_base_mem()->moved |= AP_SETS_RAW | AP_SETS_FILTER;
    }

    if (proto_back != '\0')
    {
        const char tp[] = { 'A', 'T', 'T', 'P', proto_back, '\0' };

        if (ap_base_request_tr(tp, resp, sizeof(resp), AP_FLT_SETUP_TIMEOUT,
                               tr) != ESP_OK)
        {
            ap_base_mem()->moved |= AP_SETS_PROTOCOL;
        }
    }
}

/** The protocol of the id's width for the window (the legacy firmware's
 *  rule, v4.50 `send_can_filter_cmd`): `ATCRA` matches only ids of the
 *  width of the protocol the chip is on, so a 29-bit id on a car whose
 *  protocol is the 11-bit one, and an 11-bit id behind a row whose init
 *  set `ATSP7`, were never seen (bench 2026-11-14). The sibling at the
 *  same bit rate is tried (`ATTP`: nothing is stored in the chip) when the
 *  bus guard allows it there. @return the protocol to put back behind the
 *  window, '\0' = the chip stays where it is. */
static char flt_proto_for(bool extended, ap_tr_t *tr)
{
    char was = ap_base_proto_now();
    char want = ap_flt_window_proto(was, extended);
    char resp[64];

    if (want == '\0')
    {
        return '\0';
    }

    const char tp[] = { 'A', 'T', 'T', 'P', want, '\0' };

    if (!ap_guard_chain_ok(tp, "a CAN filter's window", tr != NULL, NULL))
    {
        ap_tr_add(tr, '!', "the protocol of this ID's width is not the "
                         "vehicle bus's (the bus guard): listening on the "
                         "chip's own");
        return '\0';
    }

    if (ap_base_request_tr(tp, resp, sizeof(resp), AP_FLT_SETUP_TIMEOUT,
                           tr) != ESP_OK)
    {
        /* taken or not: the next row type starts from the baseline */
        ap_base_mem()->moved |= AP_SETS_PROTOCOL;
        return '\0';
    }

    return was;
}

/** One window: headers on, the protocol of the id's width, the hardware
 *  filter on the one id and the CAN auto formatting off (COMMAND
 *  transactions), the MONITOR claim, ATMA, a frame per wanted slot or
 *  @p monitor_ms, the stop, the chip's settings back, the runner reset.
 *  @p tr (may be NULL): the exchange, for a test. @p first_us (may be
 *  NULL): from ATMA to the first frame with the id. ESP_ERR_TIMEOUT = no
 *  such frame; ESP_ERR_INVALID_STATE = the chip is another master's. */
static esp_err_t flt_window(uint32_t frame_id, bool extended,
                            uint32_t monitor_ms, const ap_flt_want_t *want,
                            uint16_t n, ap_tr_t *tr, int64_t *first_us)
{
    uint16_t pending = 0;

    for (uint16_t i = 0; i < n; i++)
    {
        s_slot[i].have = false;

        if (want[i].want)
        {
            pending++;
        }
    }

    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(AP_FLT_Q_LEN, sizeof(obd_chunk_t),
                                 s_q_store, &s_q_buf);

        if (s_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    ap_bus_leave(tr); /* a monitor window is about the car's own bus */

    /* window setup (COMMAND transactions): headers ON so monitor lines
       carry the CAN id our parser matches, then hardware-filter to the
       one id */
    char cmd[20], resp[64];

    if (ap_base_request_tr("ATH1", resp, sizeof(resp), AP_FLT_SETUP_TIMEOUT,
                           tr) != ESP_OK)
    {
        return ESP_FAIL;
    }

    char proto_back = flt_proto_for(extended, tr);

    snprintf(cmd, sizeof(cmd), extended ? "ATCRA%08lX" : "ATCRA%03lX",
             (unsigned long)frame_id);

    if (ap_base_request_tr(cmd, resp, sizeof(resp), AP_FLT_SETUP_TIMEOUT,
                           tr) != ESP_OK)
    {
        flt_settings_back(tr, proto_back);
        return ESP_FAIL;   /* headers stay on: the CAN prelude's state */
    }

    /* the frame as it is on the bus (2026-11-14): with the chip's CAN auto
       formatting on, the baseline's state, the monitor reads a broadcast
       frame as a diagnostic message and its first byte as the length. A
       frame `02 1E 02 1F 0B 7C 4E 20` came out as `02 1E 02`, so B4 did
       not exist for its filter; any other first byte got `<DATA ERROR`
       appended. Not taken = the window as it was before */
    (void)ap_base_request_tr("ATCAF0", resp, sizeof(resp),
                             AP_FLT_SETUP_TIMEOUT, tr);

    bool got = false, listened = false;
    uint8_t payload[AP_PAYLOAD_MAX];
    size_t payload_len = 0;
    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    if (ap_be()->claim_monitor(AP_FLT_CLAIM_TIMEOUT) != ESP_OK)
    {
        flt_settings_back(tr, proto_back);
        ap_runner_reset();      /* ATCRA changed under the PID runner */
        return ESP_ERR_INVALID_STATE;
    }

    xQueueReset(s_q);

    if (ap_be()->subscribe(s_q, "autopid_flt") == ESP_OK)
    {
        ap_tr_add(tr, '>', "ATMA");

        if (ap_be()->send((const uint8_t *)"ATMA\r", 5) == ESP_OK)
        {
            ap_flt_stream_t lines;
            int64_t t0 = esp_timer_get_time();
            int64_t deadline = t0 + (int64_t)monitor_ms * 1000;

            listened = true;
            ap_flt_stream_init(&lines);

            while (pending > 0 && esp_timer_get_time() < deadline)
            {
                obd_chunk_t c;

                if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(50)) != pdTRUE)
                {
                    continue;
                }

                /* a chunk can carry several frames: resume mid-chunk
                   after every match (mux pages arrive back to back) */
                size_t off = 0;

                while (off < c.len && pending > 0)
                {
                    size_t used = 0;

                    if (!ap_flt_stream_feed_ex(&lines, c.data + off,
                                               c.len - off, frame_id,
                                               payload, sizeof(payload),
                                               &payload_len, &used))
                    {
                        break;
                    }

                    off += used;

                    if (!got && first_us != NULL)
                    {
                        *first_us = esp_timer_get_time() - t0;
                    }

                    got = true;

                    /* the frame to every still-waiting parameter */
                    uint16_t took = flt_take(want, n, payload, payload_len,
                                             (double)volts);

                    pending -= took;

                    if (took > 0 && tr != NULL)
                    {
                        char line[192];

                        ap_flt_frame_line(frame_id, payload, payload_len,
                                          line, sizeof(line));
                        ap_tr_add(tr, '<', line);
                    }
                }
            }

            window_close();
        }

        ap_be()->unsubscribe(s_q);
    }

    ap_be()->release();

    /* restore the polling shape: auto formatting on, receive-all, the
       protocol the chip was on (headers stay on, the CAN prelude's state
       since 2026-10-24) */
    flt_settings_back(tr, proto_back);
    ap_runner_reset();          /* replay PID inits on the next poll    */

    return got ? ESP_OK : listened ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

bool ap_runner_run_filter(const ap_filter_t *f, const ap_param_t *params)
{
    uint16_t n_prm = (f->param_count > AP_PARAMS_PER) ? AP_PARAMS_PER
                                                      : f->param_count;

    for (uint16_t i = 0; i < n_prm; i++)
    {
        s_want[i].mux_expr = params[i].mux_expr;
        s_want[i].mux_val = params[i].mux_val;
        s_want[i].want = params[i].enabled;
    }

    esp_err_t err = flt_window(f->frame_id, f->is_extended, f->monitor_ms,
                               s_want, n_prm, NULL, NULL);

    if (err != ESP_OK)
    {
        if (err == ESP_ERR_TIMEOUT)
        {
            ESP_LOGD(TAG, "filter %lX: no frame in %lu ms",
                     (unsigned long)f->frame_id,
                     (unsigned long)f->monitor_ms);
        }

        return false;
    }

    /* evaluate every parameter that saw its frame */
    int64_t now = esp_timer_get_time();
    bool any = false;
    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    for (uint16_t i = 0; i < n_prm; i++)
    {
        const ap_param_t *prm = &params[i];

        if (!prm->enabled || !s_slot[i].have)
        {
            continue;
        }

        double value = 0;

        if (expression_parser_eval(prm->expression, s_slot[i].data,
                                   s_slot[i].len, (double)volts,
                                   &value) != ESP_OK)
        {
            ESP_LOGD(TAG, "filter %lX/%s: expression failed",
                     (unsigned long)f->frame_id, prm->name);
            continue;
        }

        if ((!isnan(prm->min) && value < prm->min) ||
            (!isnan(prm->max) && value > prm->max))
        {
            continue;           /* plausibility clamp, like the PID path */
        }

        ap_cache_put(f->param_start + i, value, now);
        ap_events_param(prm, f->param_start + i, f->group, value);
        any = true;
    }

    return any;
}

esp_err_t ap_runner_test_filter(uint32_t frame_id, uint32_t monitor_ms,
                                const ap_flt_want_t *want, uint16_t n,
                                int64_t *elapsed_us, char *transcript,
                                size_t transcript_len)
{
    ap_tr_t tr = { transcript, transcript_len, 0 };

    if (transcript != NULL && transcript_len > 0)
    {
        transcript[0] = '\0';
    }

    if (elapsed_us != NULL)
    {
        *elapsed_us = 0;
    }

    if (n > AP_PARAMS_PER)
    {
        n = AP_PARAMS_PER;
    }

    esp_err_t err = flt_window(frame_id, frame_id > 0x7FF, monitor_ms, want,
                               n, &tr, elapsed_us);

    if (err == ESP_ERR_TIMEOUT)
    {
        char line[72];

        snprintf(line, sizeof(line), "no frame with CAN ID %lX in %lu ms",
                 (unsigned long)frame_id, (unsigned long)monitor_ms);
        ap_tr_add(&tr, '!', line);
    }
    else if (err == ESP_ERR_INVALID_STATE)
    {
        ap_tr_add(&tr, '!', "not listened: the chip is busy");
    }

    return err;
}

const uint8_t *ap_flt_slot(uint16_t i, size_t *len)
{
    if (i >= AP_PARAMS_PER || !s_slot[i].have)
    {
        return NULL;
    }

    if (len != NULL)
    {
        *len = s_slot[i].len;
    }

    return s_slot[i].data;
}
