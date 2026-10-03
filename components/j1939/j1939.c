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
 * @file j1939.c
 * @brief The J1939 component's glue: one can_manager subscription for every
 *        29-bit frame, one task that sorts the frames into the pure modules
 *        (transport protocol, message store, sources, and in active mode
 *        the address claim and the answers the node owes), the lifecycle
 *        and the counters.
 *
 * Nothing in this file transmits: active mode's frames are decided here
 * under the lock and sent by j1939_tx_flush() (j1939_tx.c) after it. The
 * receive task runs on a PSRAM stack: it only moves frames between queues
 * and PSRAM tables (no flash, no file).
 *
 * The task is created once and never deleted: j1939_stop() parks it on a
 * task notification, j1939_start() wakes it. (A static task that deletes
 * itself stays on the kernel's termination list until the idle task has
 * run; creating it again on the same memory before that corrupts the list.)
 */
#include "j1939.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "can_manager.h"
#include "log_manager.h"

#include "j1939_private.h"

static const char *TAG = "j1939";

/* 512 frames: 140 ms of a 500 kbit/s bus at line rate (3600 frames/s)
   before can_manager starts dropping the oldest for us (queue_drops) */
#define J1939_Q_LEN      512
#define J1939_TASK_STACK 4096
/* above the consumers of its data (autopid 5, the logger 3), below the
   bus's own receive task (10) */
#define J1939_TASK_PRIO  7
#define J1939_BATCH      64      /* frames taken in under one lock hold     */
#define J1939_IDLE_MS    100     /* queue wait: also the stop latency       */
#define J1939_EXPIRE_US  250000  /* how often quiet transport sessions go   */

static j1939_cache_t   s_cache EXT_RAM_BSS_ATTR; /* ~42 KB  */
static j1939_tp_t      s_tp EXT_RAM_BSS_ATTR;    /* ~14 KB  */
static j1939_sources_t s_src EXT_RAM_BSS_ATTR;   /* ~5 KB   */
static j1939_bus_evidence_t s_ev;

static struct
{
    uint32_t frames;
    uint32_t data;
    uint32_t tp_cm;
    uint32_t tp_dt;
    uint32_t diag;
    uint32_t foreign;
} s_rx;

static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;          /* internal: FreeRTOS object   */
static uint8_t s_q_store[J1939_Q_LEN * sizeof(can_core_frame_t)]
    EXT_RAM_BSS_ATTR;

static TaskHandle_t s_task;
static StaticTask_t s_tcb;             /* internal: FreeRTOS object   */
static StackType_t s_stack[J1939_TASK_STACK] EXT_RAM_BSS_ATTR; /* no flash I/O */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object   */

static int s_sub = -1;                 /* can_manager subscriber slot */
static uint32_t s_drops_base;          /* its drops at the last reset */
static uint32_t s_drops_last;          /* its drops when last read    */
static uint32_t s_seq_base;            /* store sequence at the last reset:
                                          the sequence itself never goes
                                          back (readers compare it)       */
static volatile bool s_run;
static volatile bool s_parked;
static j1939_state_t s_state = J1939_STATE_OFF;

/* ---- shared state ----------------------------------------------------------------- */

void j1939_lock(void)
{
    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void j1939_unlock(void)
{
    if (s_lock != NULL)
    {
        xSemaphoreGive(s_lock);
    }
}

j1939_cache_t *j1939_priv_cache(void)
{
    return &s_cache;
}

j1939_sources_t *j1939_priv_sources(void)
{
    return &s_src;
}

j1939_tp_t *j1939_priv_tp(void)
{
    return &s_tp;
}

const char *j1939_state_name(j1939_state_t state)
{
    switch (state)
    {
    case J1939_STATE_LISTENING: return "listening";
    case J1939_STATE_NO_BUS:    return "no_bus";
    default:                    return "off";
    }
}

/* ---- frames in -------------------------------------------------------------------- */

/** When the frame was on the wire, on the 64-bit clock. The driver stamps
 *  frames in its interrupt with the low 32 bits of that clock; the task may
 *  take them out of the queue milliseconds later. */
static int64_t frame_time(const can_core_frame_t *f, int64_t now_us)
{
    if (f->ts_us == 0)
    {
        return now_us;
    }

    return now_us - (int64_t)((uint32_t)now_us - f->ts_us);
}

/** Sort one frame. Called with the lock held. */
static void ingest(const can_core_frame_t *f, int64_t now_us)
{
    j1939_id_t id = { 0 };
    j1939_tp_msg_t msg;
    uint8_t dlc = (f->dlc <= 8) ? f->dlc : 8;
    int64_t t = frame_time(f, now_us);

    s_rx.frames++;
    j1939_bus_note(&s_ev, f->id, f->ext, f->rtr, dlc);

    switch (j1939_classify(f->id, f->ext, f->rtr, &id))
    {
    case J1939_KIND_FOREIGN:
        s_rx.foreign++;
        return;

    case J1939_KIND_DIAG:
        s_rx.diag++;
        return;

    case J1939_KIND_TP_CM:
        s_rx.tp_cm++;
        j1939_sources_note(&s_src, id.sa, t);
        j1939_tp_cm(&s_tp, id.sa, id.da, f->data, dlc, t);
        return;

    case J1939_KIND_TP_DT:
        s_rx.tp_dt++;
        j1939_sources_note(&s_src, id.sa, t);

        if (j1939_tp_dt(&s_tp, id.sa, id.da, f->data, dlc, t, &msg))
        {
            (void)j1939_cache_put(&s_cache, msg.pgn, msg.sa, msg.da, msg.data,
                                  msg.len, t);
        }

        return;

    default:
        s_rx.data++;
        j1939_sources_note(&s_src, id.sa, t);

        /* the network management groups: who is who, and what active mode
           must answer (j1939_tx.c decides under this lock, sends after it) */
        if (id.pgn == J1939_PGN_CLAIM && dlc == 8)
        {
            j1939_sources_claim(&s_src, id.sa, f->data);
            j1939_tx_on_claim(id.sa, f->data, t);
        }
        else if (id.pgn == J1939_PGN_REQUEST)
        {
            uint32_t pgn = 0;

            if (j1939_request_parse(f->data, dlc, &pgn))
            {
                j1939_tx_on_request(id.sa, id.da, pgn, t);
            }
        }
        else if (id.pgn == J1939_PGN_ACKM)
        {
            j1939_tx_on_ackm(id.sa, id.da, f->data, dlc, t);
        }

        (void)j1939_cache_put(&s_cache, id.pgn, id.sa, id.da, f->data, dlc, t);
        return;
    }
}

static void rx_task(void *arg)
{
    int64_t expire_at = 0;

    (void)arg;

    for (;;)
    {
        can_core_frame_t f;

        if (!s_run)
        {
            s_parked = true;
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            s_parked = false;
            continue;
        }

        if (xQueueReceive(s_q, &f, pdMS_TO_TICKS(J1939_IDLE_MS)) == pdTRUE)
        {
            int64_t now = esp_timer_get_time();
            unsigned n = 0;

            j1939_lock();

            do
            {
                ingest(&f, now);
            } while (++n < J1939_BATCH && xQueueReceive(s_q, &f, 0) == pdTRUE);

            j1939_unlock();
            j1939_tx_flush(); /* what the frames called for, if anything */
        }

        int64_t now = esp_timer_get_time();

        /* the clock of active mode: the claim's wait, the bus opening */
        j1939_lock();
        j1939_tx_tick(now);

        if (now >= expire_at)
        {
            j1939_tp_expire(&s_tp, now);
            expire_at = now + J1939_EXPIRE_US;
        }

        j1939_unlock();
        j1939_tx_flush();
    }
}

/* ---- lifecycle -------------------------------------------------------------------- */

esp_err_t j1939_init(void)
{
    static const log_descriptor_t LOG_DESC = { "j1939", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        j1939_cache_init(&s_cache);
        j1939_tp_init(&s_tp);
        j1939_sources_init(&s_src);
    }

    return j1939_settings_register();
}

esp_err_t j1939_start(void)
{
    if (!j1939_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE; /* §4.3 step 5 */
    }

    if (!j1939_settings_enabled())
    {
        s_state = J1939_STATE_OFF;
        ESP_LOGI(TAG, "disabled by settings");
        return ESP_OK;
    }

    if (s_state == J1939_STATE_LISTENING)
    {
        return ESP_OK;
    }

    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(J1939_Q_LEN, sizeof(can_core_frame_t),
                                 s_q_store, &s_q_buf);
    }

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(rx_task, "j1939", J1939_TASK_STACK, NULL,
                                   J1939_TASK_PRIO, s_stack, &s_tcb);
    }

    if (s_q == NULL || s_task == NULL || s_lock == NULL)
    {
        ESP_LOGE(TAG, "queue / task / lock not created");
        return ESP_FAIL;
    }

    /* every 29-bit frame: the group number is spread over the identifier,
       a hardware-style mask cannot pick "J1939" */
    esp_err_t err = can_manager_subscribe_queue(s_q, 0, 0, true, false,
                                                &s_sub);

    if (err == ESP_ERR_INVALID_STATE)
    {
        s_sub = -1;
        s_state = J1939_STATE_NO_BUS;
        ESP_LOGW(TAG, "enabled, but the native CAN bus is off "
                      "(can_manager.enabled): nothing to listen to");
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        s_sub = -1;
        ESP_LOGE(TAG, "no CAN subscription (%s)", esp_err_to_name(err));
        return err;
    }

    (void)can_manager_subscriber_name(s_sub, "j1939");
    s_drops_base = 0;
    s_drops_last = 0;
    xQueueReset(s_q);
    j1939_lock();
    j1939_tx_start();
    j1939_unlock();
    s_run = true;
    xTaskNotifyGive(s_task);
    s_state = J1939_STATE_LISTENING;

    if (j1939_settings_active())
    {
        ESP_LOGI(TAG, "listening, active mode: address %u will be claimed "
                      "once the bus lets this node transmit; %u message "
                      "slots, %u long buffers, %u transport sessions",
                 j1939_settings_address(), (unsigned)J1939_CACHE_MAX,
                 (unsigned)J1939_LONG_SLOTS, (unsigned)J1939_TP_SESSIONS);
    }
    else
    {
        ESP_LOGI(TAG, "listening, never transmits: %u message slots, %u long "
                      "buffers, %u transport sessions",
                 (unsigned)J1939_CACHE_MAX, (unsigned)J1939_LONG_SLOTS,
                 (unsigned)J1939_TP_SESSIONS);
    }

    return ESP_OK;
}

esp_err_t j1939_stop(void)
{
    if (s_state != J1939_STATE_LISTENING)
    {
        s_state = J1939_STATE_OFF;
        return ESP_OK;
    }

    /* keep the drop count readable after the subscription is gone */
    can_manager_subscriber_t sub;

    if (can_manager_subscriber_get(s_sub, &sub))
    {
        s_drops_last = sub.drops;
    }

    s_run = false;
    (void)can_manager_unsubscribe_queue(s_sub);
    s_sub = -1;

    for (int i = 0; i < 50 && !s_parked; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    j1939_lock();
    j1939_tx_stop(); /* the address is dropped with the session */
    j1939_unlock();
    s_state = J1939_STATE_OFF;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

/* ---- status ----------------------------------------------------------------------- */

esp_err_t j1939_status(j1939_status_t *out)
{
    can_manager_subscriber_t sub;

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (s_sub >= 0 && can_manager_subscriber_get(s_sub, &sub))
    {
        s_drops_last = sub.drops;
    }

    j1939_lock();
    out->state = s_state;
    out->bus = j1939_bus_verdict(&s_ev);
    out->rx_frames = s_rx.frames;
    out->rx_data = s_rx.data;
    out->rx_tp_cm = s_rx.tp_cm;
    out->rx_tp_dt = s_rx.tp_dt;
    out->rx_diag = s_rx.diag;
    out->rx_foreign = s_rx.foreign;
    out->queue_drops = s_drops_last - s_drops_base;
    out->messages = s_cache.seq - s_seq_base;
    out->not_kept = s_cache.full;
    out->evicted = s_cache.evicted;
    out->long_evicted = s_cache.lng_evicted;
    out->entries = s_cache.used;
    out->entries_cap = J1939_CACHE_MAX;
    out->sources = (uint16_t)j1939_sources_count(&s_src);
    out->tp_open = (uint16_t)j1939_tp_open(&s_tp);
    out->tp_cap = J1939_TP_SESSIONS;
    out->tp_started = s_tp.stats.started;
    out->tp_completed = s_tp.stats.completed;
    out->tp_seq_errors = s_tp.stats.seq_errors;
    out->tp_timeouts = s_tp.stats.timeouts;
    out->tp_aborted = s_tp.stats.aborted;
    out->tp_replaced = s_tp.stats.replaced;
    out->tp_no_session = s_tp.stats.no_session;
    out->tp_orphan_dt = s_tp.stats.orphan_dt;
    out->tp_bad_cm = s_tp.stats.bad_cm;
    j1939_tx_status(out);
    j1939_unlock();
    return ESP_OK;
}

uint32_t j1939_sequence(void)
{
    return s_cache.seq;
}

void j1939_reset(void)
{
    can_manager_subscriber_t sub;

    if (s_sub >= 0 && can_manager_subscriber_get(s_sub, &sub))
    {
        s_drops_last = sub.drops;
    }

    j1939_lock();
    memset(&s_rx, 0, sizeof(s_rx));
    memset(&s_ev, 0, sizeof(s_ev));

    uint32_t seq = s_cache.seq + 1u; /* "something changed" for a reader */

    j1939_cache_init(&s_cache);
    s_cache.seq = seq;
    s_seq_base = seq;
    uint8_t my_sa = s_tp.my_sa; /* the sessions go, the address stays */

    j1939_tp_init(&s_tp);
    j1939_tp_set_address(&s_tp, my_sa);
    j1939_sources_init(&s_src);
    j1939_tx_reset();
    s_drops_base = s_drops_last;
    j1939_unlock();
}

void j1939_capacity(size_t *used, size_t *cap)
{
    if (used != NULL)
    {
        *used = s_cache.used;
    }

    if (cap != NULL)
    {
        *cap = J1939_CACHE_MAX;
    }
}

void j1939_tp_capacity(size_t *used, size_t *cap)
{
    if (used != NULL)
    {
        j1939_lock();
        *used = j1939_tp_open(&s_tp);
        j1939_unlock();
    }

    if (cap != NULL)
    {
        *cap = J1939_TP_SESSIONS;
    }
}
