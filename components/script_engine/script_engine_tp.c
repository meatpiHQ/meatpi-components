/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_tp.c
 * @brief ISO-TP sessions and raw frames for the flash bindings, on the
 *        script runner task. Each session owns one can_manager queue
 *        subscription on its reply ID; the raw path owns one more, opened
 *        at the first flash_can_send/recv/quiet. All are closed after every
 *        run, and when the script stops the bus (can_stop wipes the
 *        subscriber table, so sessions opened before it are gone).
 *
 * Pacing: consecutive frames go out no closer than max(the ECU's STmin,
 * the session's gap_us), measured from each frame's hand-off to the
 * driver. Under 2 ms the wait spins (esp_rom_delay_us); longer waits
 * sleep. A gap is a minimum: preemption can only stretch it.
 */
#include "script_engine_flash.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_manager.h"

static const char *TAG = "script_engine";

#define SE_TP_QLEN    128 /* frames per session queue (drop-oldest)      */
#define SE_RAW_QLEN   128
#define SE_FC_WAITS   10  /* WAIT flow controls tolerated per block       */

typedef struct
{
    bool         used;
    uint32_t     tx_id;
    uint32_t     rx_id;
    bool         ext;
    uint32_t     gap_us;
    se_tp_opts_t o;
    int          sub;
    QueueHandle_t q;
    se_tp_rx_t   rx;
} se_tp_session_t;

EXT_RAM_BSS_ATTR static se_tp_session_t s_tp[SE_TP_SESSIONS];
EXT_RAM_BSS_ATTR static uint8_t s_tp_buf[SE_TP_SESSIONS][SE_TP_MAX];
EXT_RAM_BSS_ATTR static uint8_t s_tp_qstore[SE_TP_SESSIONS]
                                           [SE_TP_QLEN * sizeof(can_core_frame_t)];
static StaticQueue_t s_tp_qobj[SE_TP_SESSIONS]; /* internal: FreeRTOS object */

EXT_RAM_BSS_ATTR static uint8_t s_raw_qstore[SE_RAW_QLEN * sizeof(can_core_frame_t)];
static StaticQueue_t s_raw_qobj;                /* internal: FreeRTOS object */
static QueueHandle_t s_raw_q;
static int s_raw_sub = -1;

static TickType_t ms_ticks(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);

    return (ms > 0 && t == 0) ? 1 : t;
}

static uint32_t ms_left(int64_t deadline_us)
{
    int64_t left = deadline_us - esp_timer_get_time();

    return (left <= 0) ? 0 : (uint32_t)((left + 999) / 1000);
}

int se_tp_open(uint32_t tx_id, uint32_t rx_id, bool ext,
               const se_tp_opts_t *o, uint32_t gap_us)
{
    for (int h = 0; h < SE_TP_SESSIONS; h++)
    {
        se_tp_session_t *s = &s_tp[h];

        if (s->used)
        {
            continue;
        }

        if (s->q == NULL)
        {
            s->q = xQueueCreateStatic(SE_TP_QLEN, sizeof(can_core_frame_t),
                                      s_tp_qstore[h], &s_tp_qobj[h]);
        }

        xQueueReset(s->q);

        uint32_t mask = ext ? 0x1FFFFFFFU : 0x7FFU;
        int idx = -1;
        esp_err_t err = can_manager_subscribe_queue(s->q, rx_id, mask, ext,
                                                    false, &idx);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "flash session: no CAN subscription (%s)",
                     esp_err_to_name(err));
            return -1;
        }

        (void)can_manager_subscriber_name(idx, "script flash");
        s->used = true;
        s->tx_id = tx_id;
        s->rx_id = rx_id;
        s->ext = ext;
        s->gap_us = gap_us;
        s->o = *o;
        s->sub = idx;
        se_tp_rx_init(&s->rx, s_tp_buf[h], SE_TP_MAX);
        return h;
    }

    return -1;
}

void se_tp_close(int h)
{
    if (h < 0 || h >= SE_TP_SESSIONS || !s_tp[h].used)
    {
        return;
    }

    (void)can_manager_unsubscribe_queue(s_tp[h].sub);
    s_tp[h].used = false;
    s_tp[h].sub = -1;
}

static se_tp_session_t *session(int h)
{
    return (h >= 0 && h < SE_TP_SESSIONS && s_tp[h].used) ? &s_tp[h] : NULL;
}

void se_tp_flush(int h)
{
    se_tp_session_t *s = session(h);

    if (s != NULL)
    {
        xQueueReset(s->q);
        se_tp_rx_init(&s->rx, s->rx.buf, s->rx.cap);
    }
}

/* Hand one frame to the driver; a full TX queue is waited out briefly. */
static esp_err_t put(uint32_t id, bool ext, const uint8_t *d, uint8_t dlc)
{
    int64_t until = esp_timer_get_time() + 50000;

    while (true)
    {
        esp_err_t err = can_manager_send(id, ext, false, d, dlc);

        if (err != ESP_FAIL || esp_timer_get_time() > until)
        {
            return err;
        }

        esp_rom_delay_us(100); /* the driver's 16 slots are full */
    }
}

/* Wait until @p due (µs since boot): spin the last 2 ms, sleep before. */
static void wait_until(int64_t due)
{
    int64_t now = esp_timer_get_time();

    while (due - now > 2000)
    {
        vTaskDelay(1);
        now = esp_timer_get_time();
    }

    if (due > now)
    {
        esp_rom_delay_us((uint32_t)(due - now));
    }
}

/* The next flow control for @p s, or an error. */
static esp_err_t await_fc(se_tp_session_t *s, int64_t deadline,
                          uint8_t *bs, uint32_t *stmin_us)
{
    int waits = 0;
    can_core_frame_t f;

    while (true)
    {
        /* a deadline that passed still looks at the queue once */
        if (xQueueReceive(s->q, &f, ms_ticks(ms_left(deadline))) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        switch (se_tp_parse_fc(&s->o, f.data, f.dlc, bs, stmin_us))
        {
            case SE_TP_FC_CTS:
                return ESP_OK;
            case SE_TP_FC_WAIT:
                if (++waits > SE_FC_WAITS)
                {
                    return ESP_ERR_TIMEOUT;
                }
                deadline = esp_timer_get_time() + 1000000; /* N_Bs again */
                break;
            case SE_TP_FC_OVERFLOW:
                return ESP_ERR_INVALID_SIZE;
            default:
                break; /* not a flow control: not for the send path */
        }
    }
}

esp_err_t se_tp_send(int h, const uint8_t *msg, size_t len,
                     uint32_t timeout_ms)
{
    se_tp_session_t *s = session(h);
    uint8_t fr[8];
    size_t off = 0;

    if (s == NULL || msg == NULL || len == 0 || len > SE_TP_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t dlc = se_tp_first(&s->o, msg, len, fr, &off);
    esp_err_t err = put(s->tx_id, s->ext, fr, dlc);

    if (err != ESP_OK || off >= len)
    {
        return err;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t sn = 1;

    while (off < len)
    {
        uint8_t bs = 0;
        uint32_t stmin_us = 0;

        err = await_fc(s, deadline, &bs, &stmin_us);

        if (err != ESP_OK)
        {
            return err;
        }

        uint32_t gap = (stmin_us > s->gap_us) ? stmin_us : s->gap_us;
        int64_t due = esp_timer_get_time();
        unsigned sent = 0;

        while (off < len && (bs == 0 || sent < bs))
        {
            wait_until(due);
            dlc = se_tp_consecutive(&s->o, msg, len, &off, sn, fr);
            err = put(s->tx_id, s->ext, fr, dlc);

            if (err != ESP_OK)
            {
                return err;
            }

            due = esp_timer_get_time() + gap;
            sn = (uint8_t)((sn + 1) & 0x0F);
            sent++;
        }

        deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    }

    return ESP_OK;
}

esp_err_t se_tp_recv(int h, const uint8_t **out, size_t *len,
                     uint32_t timeout_ms)
{
    se_tp_session_t *s = session(h);
    can_core_frame_t f;

    if (s == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (true)
    {
        /* a deadline that passed (or 0 ms) still looks at the queue once */
        if (xQueueReceive(s->q, &f, ms_ticks(ms_left(deadline))) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        se_tp_rx_status_t st = se_tp_rx_feed(&s->rx, &s->o, f.data, f.dlc);

        if (st == SE_TP_RX_NEED_FC)
        {
            uint8_t fc[8];
            uint8_t dlc = se_tp_flow_control(&s->o, fc);
            esp_err_t err = put(s->tx_id, s->ext, fc, dlc);

            if (err != ESP_OK)
            {
                return err;
            }

            /* a multi-frame reply: the timeout applies again per block */
            deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        }
        else if (st == SE_TP_RX_MORE)
        {
            /* N_Cr: the timeout runs between consecutive frames, not over
               the whole message (a 4 KiB reply is 585 of them) */
            deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        }
        else if (st == SE_TP_RX_DONE)
        {
            *out = s->rx.buf;
            *len = s->rx.len;
            return ESP_OK;
        }
        else if (st == SE_TP_RX_ERROR)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
}

/* ---- raw frames ------------------------------------------------------------ */

static esp_err_t raw_open(void)
{
    if (s_raw_sub >= 0)
    {
        return ESP_OK;
    }

    if (s_raw_q == NULL)
    {
        s_raw_q = xQueueCreateStatic(SE_RAW_QLEN, sizeof(can_core_frame_t),
                                     s_raw_qstore, &s_raw_qobj);
    }

    xQueueReset(s_raw_q);

    esp_err_t err = can_manager_subscribe_queue(s_raw_q, 0, 0, false, true,
                                                &s_raw_sub);

    if (err != ESP_OK)
    {
        s_raw_sub = -1;
        return err;
    }

    (void)can_manager_subscriber_name(s_raw_sub, "script flash raw");
    return ESP_OK;
}

/* The raw subscription exists before the frame goes out, so a fast reply
   is never missed; frames queued before the send are dropped, so a recv
   after it reads what came after it (flash_request does the same). */
esp_err_t se_raw_send(uint32_t id, bool ext, const uint8_t *d, uint8_t dlc)
{
    if (dlc > 8)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = raw_open();

    if (err != ESP_OK)
    {
        return err;
    }

    xQueueReset(s_raw_q);
    return put(id, ext, d, dlc);
}

static bool id_match(const uint32_t *ids, size_t n, uint32_t id)
{
    if (n == 0)
    {
        return true;
    }

    for (size_t i = 0; i < n; i++)
    {
        if (ids[i] == id)
        {
            return true;
        }
    }

    return false;
}

esp_err_t se_raw_recv(const uint32_t *ids, size_t n_ids, uint32_t timeout_ms,
                      can_core_frame_t *out)
{
    esp_err_t err = raw_open();

    if (err != ESP_OK)
    {
        return err;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (true)
    {
        if (xQueueReceive(s_raw_q, out, ms_ticks(ms_left(deadline))) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        if (id_match(ids, n_ids, out->id))
        {
            return ESP_OK;
        }
    }
}

bool se_raw_quiet(const uint32_t *ids, size_t n_ids, uint32_t window_ms,
                  uint32_t timeout_ms)
{
    if (raw_open() != ESP_OK)
    {
        return false;
    }

    int64_t now = esp_timer_get_time();
    int64_t deadline = now + (int64_t)timeout_ms * 1000;
    int64_t quiet_until = now + (int64_t)window_ms * 1000;
    can_core_frame_t f;

    while (true)
    {
        now = esp_timer_get_time();

        if (now >= quiet_until)
        {
            return true;
        }

        if (now >= deadline)
        {
            return false;
        }

        int64_t until = (quiet_until < deadline) ? quiet_until : deadline;
        uint32_t wait = ms_left(until);

        if (xQueueReceive(s_raw_q, &f, ms_ticks(wait)) == pdTRUE &&
            id_match(ids, n_ids, f.id))
        {
            quiet_until = esp_timer_get_time() + (int64_t)window_ms * 1000;
        }
    }
}

void se_tp_bus_down(void)
{
    /* the subscriber table is wiped when the bus stops: let go of the
       slots while they are still ours, so a later close cannot unsubscribe
       whoever takes them after the restart */
    se_tp_epilogue();
}

void se_tp_epilogue(void)
{
    for (int h = 0; h < SE_TP_SESSIONS; h++)
    {
        se_tp_close(h);
    }

    if (s_raw_sub >= 0)
    {
        (void)can_manager_unsubscribe_queue(s_raw_sub);
        s_raw_sub = -1;
    }
}
