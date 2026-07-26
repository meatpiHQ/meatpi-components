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
 * @file can_core.c
 * @brief Shared CAN bus manager for the ELM327 emulator.
 *
 * Wraps the ESP-IDF TWAI (Two-Wire Automotive Interface) driver and provides
 * multi-client frame dispatch so that multiple AT engine instances can share
 * one physical CAN bus.
 *
 * Architecture
 * ------------
 * - One FreeRTOS task runs per can_core_handle_t to service the TWAI RX
 *   queue (on-device only).
 * - Each received frame is compared against every registered client's filter
 *   and mask.  Matching clients have their rx_cb invoked from the RX task.
 * - TX is performed synchronously from the caller's context.
 *
 * Host (non-ESP) build
 * --------------------
 * When built outside the ESP-IDF (e.g. for pytest / unit tests) the TWAI
 * driver calls are stubbed out so that the logic can be exercised without
 * real hardware.  The RX task is replaced by a no-op.
 */

#include <string.h>
#include <stdlib.h>
#include "can_core.h"
#include "can_core_filter.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "can_core";

/* RX task stack size (bytes) */
#define CAN_CORE_RX_TASK_STACK   4096
#define CAN_CORE_RX_TASK_PRIO    10

static elm327_err_t can_driver_start(can_core_handle_t *handle,
                                     const can_core_config_t *config);
static void can_driver_stop(can_core_handle_t *handle);
#ifdef ESP_PLATFORM
static void can_rx_wait_parked(can_core_handle_t *handle);
#endif
static bool can_subscription_matches_frame(uint32_t filter,
                                           uint32_t mask,
                                           bool ext,
                                           bool monitor_all,
                                           const can_core_frame_t *frame);
static bool dispatch_frame_to_queue_subscriber(QueueHandle_t queue,
                                               const can_core_frame_t *frame);

/* ---- esp_driver_twai node state (port 2026-07-21) ------------------------
 * RX frames are copied OUT of the driver in the on_rx_done ISR into a
 * STATIC queue that can_core owns and NEVER deletes: a reader blocked
 * on it cannot race a driver teardown -- the class of the 2026-07-21
 * sleep-entry panic (twai_receive vs twai_driver_uninstall) is
 * structurally impossible with this shape. */
#define CAN_CORE_RXQ_DEPTH 64
#define CAN_RX_RAW_EXT     0x1
#define CAN_RX_RAW_RTR     0x2

typedef struct
{
    uint32_t id;
    uint8_t  flags;
    uint8_t  dlc;
    uint8_t  data[8];
} can_rx_raw_t;

static twai_node_handle_t s_node;
static StaticQueue_t      s_rxq_buf;              /* internal: FreeRTOS */
static uint8_t            s_rxq_store[CAN_CORE_RXQ_DEPTH *
                                      sizeof(can_rx_raw_t)];
static QueueHandle_t      s_rx_q;
static volatile bool      s_evt_bus_off;
static volatile bool      s_evt_recovered;

static bool IRAM_ATTR can_on_rx_done(twai_node_handle_t node,
                                     const twai_rx_done_event_data_t *e,
                                     void *ctx)
{
    can_core_handle_t *handle = (can_core_handle_t *)ctx;
    BaseType_t hp = pdFALSE;
    uint8_t buf[8];
    twai_frame_t f = { .buffer = buf, .buffer_len = sizeof(buf) };

    (void)e;

    /* ONE receive per on_rx_done event: the driver fires the callback
     * per frame, and re-calling receive_from_isr re-reads the SAME
     * frame (found at full 500k line rate 2026-07-21: a drain loop
     * here duplicated frames ~30x into the queue) */
    if (twai_node_receive_from_isr(node, &f) == ESP_OK)
    {
        can_rx_raw_t raw;

        raw.id    = f.header.id;
        raw.flags = (uint8_t)((f.header.ide ? CAN_RX_RAW_EXT : 0) |
                              (f.header.rtr ? CAN_RX_RAW_RTR : 0));
        raw.dlc   = (uint8_t)(f.header.dlc > 8 ? 8 : f.header.dlc);
        memcpy(raw.data, buf, raw.dlc);

        if (xQueueSendFromISR(s_rx_q, &raw, &hp) != pdTRUE)
        {
            handle->stats.rx_missed++;
        }

    }

    return hp == pdTRUE;
}

static bool IRAM_ATTR can_on_state_change(
    twai_node_handle_t node, const twai_state_change_event_data_t *e,
    void *ctx)
{
    (void)node;
    (void)ctx;

    if (e->new_sta == TWAI_ERROR_BUS_OFF)
    {
        s_evt_bus_off = true;
    }
    else if (e->old_sta == TWAI_ERROR_BUS_OFF)
    {
        s_evt_recovered = true;
    }

    return false;
}

/* ---- ISR core affinity (2026-07-21 experiment) ---------------------------
 * esp_intr_alloc binds the node ISR to the CALLING core. The default
 * (core 0) inherits WiFi/BT/USB/i2c neighbours and their slot pool;
 * CONFIG_WICAN_CAN_ISR_CORE=1 runs the driver init on a pinned
 * one-shot task so the CAN interrupt (and rx task) live on core 1. */
typedef struct
{
    can_core_handle_t       *handle;
    const can_core_config_t *config;
    elm327_err_t             result;
    SemaphoreHandle_t        done;
} can_start_ctx_t;

static void can_driver_start_trampoline(void *arg)
{
    can_start_ctx_t *ctx = (can_start_ctx_t *)arg;

    ctx->result = can_driver_start(ctx->handle, ctx->config);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static elm327_err_t can_driver_start_on_core(can_core_handle_t *handle,
                                             const can_core_config_t *config)
{
#if CONFIG_WICAN_CAN_ISR_CORE == 0
    return can_driver_start(handle, config);
#else
    static StaticSemaphore_t s_done_buf;
    can_start_ctx_t ctx =
    {
        .handle = handle,
        .config = config,
        .result = ELM327_ERR_CAN,
        .done = xSemaphoreCreateBinaryStatic(&s_done_buf),
    };

    if (xTaskCreatePinnedToCore(can_driver_start_trampoline, "can_init",
                                3072, &ctx, 10, NULL,
                                CONFIG_WICAN_CAN_ISR_CORE) != pdPASS)
    {
        return can_driver_start(handle, config); /* fallback: this core */
    }

    (void)xSemaphoreTake(ctx.done, portMAX_DELAY);
    return ctx.result;
#endif
}

/* -------------------------------------------------------------------------
 * Bus-off recovery servicing (runs in the RX task loop)
 *
 * Node-API flow (esp_driver_twai, 2026-07-21): on_state_change(BUS_OFF)
 * sets s_evt_bus_off -> this loop calls twai_node_recover() (completes
 * once the bus shows 128 x 11 recessive bits; the node REJOINS on its
 * own) -> on_state_change(ERR_ACTIVE) sets s_evt_recovered -> the
 * restart branch is bookkeeping + the policy backoff (thrash guard,
 * see can_core_recovery.h). CANREC-verified at 0.1 s (legacy driver's
 * initiate/stop/start dance took 0.6 s).
 * ------------------------------------------------------------------------- */
static void can_service_recovery(can_core_handle_t *handle)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_evt_bus_off)
    {
        s_evt_bus_off = false;
        handle->stats.bus_off_count++;

        uint32_t backoff =
            can_core_recovery_on_bus_off(&handle->recovery, now_ms);

        ESP_LOGE(TAG, "BUS-OFF #%lu -- initiating recovery "
                 "(restart backoff %lu ms)",
                 (unsigned long)handle->recovery.off_count,
                 (unsigned long)backoff);
        (void)twai_node_recover(s_node);
    }

    if (s_evt_recovered)
    {
        s_evt_recovered = false;
        can_core_recovery_on_recovered(&handle->recovery, now_ms);
        ESP_LOGW(TAG, "bus recovered -- restarting in %lu ms",
                 (unsigned long)handle->recovery.backoff_ms);
    }

    if (can_core_recovery_restart_due(&handle->recovery, now_ms))
    {
        /* node API: recovery re-joins the bus by itself once the 128
         * bus-idle sequences pass -- the legacy twai_start became
         * bookkeeping (the CANREC backoff geometry is preserved) */
        handle->stats.recovery_count++;
        ESP_LOGW(TAG, "bus restarted after bus-off (recovery #%lu)",
                 (unsigned long)handle->stats.recovery_count);
    }
}

/* -------------------------------------------------------------------------
 * Internal RX task
 * Runs until handle->initialised is cleared by can_core_deinit().
 * ------------------------------------------------------------------------- */
static void can_rx_task(void *arg)
{
    can_core_handle_t *handle = (can_core_handle_t *)arg;

    while (handle->initialised)
    {
        if (handle->reconfiguring)
        {
            /* rx_parked is the teardown rendezvous: the driver may only
             * be uninstalled while this task is provably parked here —
             * a twai_receive blocked on the RX queue when the queue is
             * deleted dies on its spinlock (hit at sleep entry
             * 2026-07-20) */
            handle->rx_parked = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        handle->rx_parked = false;

        can_service_recovery(handle);

        can_rx_raw_t raw;

        /* OUR queue (static, never deleted) -- 100 ms slices so the
         * initialised flag stays responsive */
        if (xQueueReceive(s_rx_q, &raw, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }

        handle->stats.rx_count++;

        /* Build our frame structure */
        can_core_frame_t frame;
        frame.id  = raw.id;
        frame.ext = (raw.flags & CAN_RX_RAW_EXT) != 0;
        frame.rtr = (raw.flags & CAN_RX_RAW_RTR) != 0;
        frame.dlc = raw.dlc;
        memcpy(frame.data, raw.data, frame.dlc);

        /* Dispatch to matching clients */
        for (int i = 0; i < CAN_CORE_MAX_CLIENTS; i++)
        {
            can_core_client_t *client = &handle->clients[i];
            if (!client->active || !client->rx_cb)
            {
                continue;
            }
            if (can_subscription_matches_frame(client->filter,
                                               client->mask,
                                               client->ext,
                                               client->monitor_all,
                                               &frame))
            {
                client->rx_cb(client->rx_ctx, &frame);
            }
        }

        for (int i = 0; i < CAN_CORE_MAX_QUEUE_SUBSCRIBERS; i++)
        {
            can_core_queue_subscriber_t *subscriber =
                &handle->queue_subscribers[i];

            if (!subscriber->active || subscriber->queue == NULL)
            {
                continue;
            }

            if (can_subscription_matches_frame(subscriber->filter,
                                               subscriber->mask,
                                               subscriber->ext,
                                               subscriber->monitor_all,
                                               &frame))
            {
                if (!dispatch_frame_to_queue_subscriber(subscriber->queue,
                                                        &frame))
                {
                    handle->stats.dispatch_drops++;
                }
            }
        }
    }

    handle->rx_exited = true; /* deinit may stop the driver now */
    vTaskDelete(NULL);
}

#endif /* ESP_PLATFORM */

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * can_core_init
 * ------------------------------------------------------------------------- */
elm327_err_t can_core_init(can_core_handle_t *handle,
                              const can_core_config_t *config)
{
    if (!handle || !config)
    {
        return ELM327_ERR_INVALID_ARG;
    }

    memset(handle, 0, sizeof(*handle));
    handle->config = *config;

    handle->initialised = true;

#ifdef ESP_PLATFORM
    elm327_err_t err = can_driver_start_on_core(handle, config);
    if (err != ELM327_OK)
    {
        handle->initialised = false;
        return err;
    }

    /* Start the RX dispatch task. WiCAN: PSRAM stack (the task does
     * twai_receive + dispatch only, never flash) + internal TCB —
     * keeps the scarce internal heap for NVS-touching tasks. */
    static StaticTask_t s_rx_tcb;
    static EXT_RAM_BSS_ATTR StackType_t
        s_rx_stack[CAN_CORE_RX_TASK_STACK];
    TaskHandle_t task_hdl =
        xTaskCreateStaticPinnedToCore(can_rx_task, "can_core_rx",
                                      CAN_CORE_RX_TASK_STACK, handle,
                                      CAN_CORE_RX_TASK_PRIO, s_rx_stack,
                                      &s_rx_tcb,
                                      CONFIG_WICAN_CAN_ISR_CORE);
    if (task_hdl == NULL)
    {
        ESP_LOGE(TAG, "rx task create failed");
        handle->initialised = false;
        can_driver_stop(handle);
        return ELM327_ERR_CAN;
    }
    handle->rx_task_handle = (void *)task_hdl;

#else
    /* Host stub */
#endif

    return ELM327_OK;
}

/* -------------------------------------------------------------------------
 * can_core_deinit
 * ------------------------------------------------------------------------- */
void can_core_deinit(can_core_handle_t *handle)
{
    if (!handle || !handle->initialised)
    {
        return;
    }

    handle->initialised = false;

#ifdef ESP_PLATFORM
    /* The RX task may be BLOCKED inside twai_receive (100 ms slices) —
     * uninstalling the driver under it deletes the RX queue it sleeps
     * on (spinlock assert; crashed every sleep entry 2026-07-20). Wait
     * for its exit ack, THEN stop the driver. Worst case one receive
     * slice + dispatch; 1 s is a hang, log and bail out anyway. */
    for (int i = 0; i < 100 && !handle->rx_exited; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!handle->rx_exited)
    {
        ESP_LOGE(TAG, "rx task did not exit; uninstalling anyway");
    }
    vTaskDelay(pdMS_TO_TICKS(20)); /* let vTaskDelete(NULL) finish
                                    * before the static TCB is reusable */
    can_driver_stop(handle);
#endif

    memset(handle->clients, 0, sizeof(handle->clients));
    memset(handle->queue_subscribers, 0, sizeof(handle->queue_subscribers));
}

/* -------------------------------------------------------------------------
 * can_core_register_client
 * ------------------------------------------------------------------------- */
int can_core_register_client(can_core_handle_t *handle,
                                const can_core_client_t *client)
{
    if (!handle || !client)
    {
        return ELM327_ERR_INVALID_ARG;
    }

    for (int i = 0; i < CAN_CORE_MAX_CLIENTS; i++)
    {
        if (!handle->clients[i].active)
        {
            handle->clients[i]        = *client;
            handle->clients[i].active = true;
            return i;
        }
    }

    return ELM327_ERR_NO_MEM;
}

int can_core_register_rx_queue(can_core_handle_t *handle,
                                 const can_core_queue_subscriber_t *subscriber)
{
    if (!handle || !subscriber || subscriber->queue == NULL)
    {
        return ELM327_ERR_INVALID_ARG;
    }

    for (int i = 0; i < CAN_CORE_MAX_QUEUE_SUBSCRIBERS; i++)
    {
        if (!handle->queue_subscribers[i].active)
        {
            handle->queue_subscribers[i] = *subscriber;
            handle->queue_subscribers[i].active = true;
            return i;
        }
    }

    return ELM327_ERR_NO_MEM;
}

/* -------------------------------------------------------------------------
 * can_core_unregister_client
 * ------------------------------------------------------------------------- */
void can_core_unregister_client(can_core_handle_t *handle, int idx)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_CLIENTS)
    {
        return;
    }
    memset(&handle->clients[idx], 0, sizeof(handle->clients[idx]));
}

void can_core_unregister_rx_queue(can_core_handle_t *handle, int idx)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS)
    {
        return;
    }

    memset(&handle->queue_subscribers[idx], 0,
           sizeof(handle->queue_subscribers[idx]));
}

/* -------------------------------------------------------------------------
 * can_core_set_filter
 * ------------------------------------------------------------------------- */
void can_core_set_filter(can_core_handle_t *handle,
                            int idx,
                            uint32_t filter,
                            uint32_t mask,
                            bool ext)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_CLIENTS)
    {
        return;
    }
    handle->clients[idx].filter = filter;
    handle->clients[idx].mask   = mask;
    handle->clients[idx].ext    = ext;
}

void can_core_set_rx_queue_filter(can_core_handle_t *handle,
                                    int idx,
                                    uint32_t filter,
                                    uint32_t mask,
                                    bool ext)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS)
    {
        return;
    }

    handle->queue_subscribers[idx].filter = filter;
    handle->queue_subscribers[idx].mask = mask;
    handle->queue_subscribers[idx].ext = ext;
}

/* -------------------------------------------------------------------------
 * can_core_set_monitor_all
 * ------------------------------------------------------------------------- */
void can_core_set_monitor_all(can_core_handle_t *handle,
                                 int idx,
                                 bool monitor_all)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_CLIENTS)
    {
        return;
    }
    handle->clients[idx].monitor_all = monitor_all;
}

void can_core_set_rx_queue_monitor_all(can_core_handle_t *handle,
                                         int idx,
                                         bool monitor_all)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS)
    {
        return;
    }

    handle->queue_subscribers[idx].monitor_all = monitor_all;
}

/* -------------------------------------------------------------------------
 * can_core_transmit
 * ------------------------------------------------------------------------- */
elm327_err_t can_core_transmit(can_core_handle_t *handle,
                                  const can_core_frame_t *frame,
                                  uint32_t timeout_ms)
{
    if (!handle || !frame || !handle->initialised)
    {
        return ELM327_ERR_INVALID_ARG;
    }

#ifdef ESP_PLATFORM
    uint8_t buf[8];
    twai_frame_t f = { 0 };

    f.header.id  = frame->id;
    f.header.ide = frame->ext ? 1 : 0;
    f.header.rtr = frame->rtr ? 1 : 0;
    f.header.dlc = frame->dlc;

    if (!frame->rtr)
    {
        memcpy(buf, frame->data, frame->dlc);
        f.buffer     = buf;
        f.buffer_len = frame->dlc;
    }

    esp_err_t ret = twai_node_transmit(s_node, &f, (int)timeout_ms);
    if (ret != ESP_OK)
    {
        handle->stats.tx_errors++;
        return (ret == ESP_ERR_TIMEOUT) ? ELM327_ERR_BUSY : ELM327_ERR_CAN;
    }

    handle->stats.tx_count++;
#else
    /* Host stub — pretend TX succeeded */
    (void)timeout_ms;
    handle->stats.tx_count++;
#endif

    return ELM327_OK;
}

/* -------------------------------------------------------------------------
 * can_core_get_stats
 * ------------------------------------------------------------------------- */
void can_core_get_stats(can_core_handle_t *handle,
                           can_core_stats_t *stats)
{
    if (!handle || !stats)
    {
        return;
    }

#ifdef ESP_PLATFORM
    twai_node_status_t info;
    twai_node_record_t rec;
    if (s_node != NULL &&
        twai_node_get_info(s_node, &info, &rec) == ESP_OK)
    {
        handle->stats.bus_errors = rec.bus_err_num;
        handle->stats.tx_errors  = info.tx_error_count;
        handle->stats.rx_errors  = info.rx_error_count;

        switch (info.state)
        {
            case TWAI_ERROR_BUS_OFF:
                handle->stats.bus_state = CAN_CORE_BUS_OFF;
                break;
            default:
                /* the node stays enabled through recovery -- RECOVERING
                 * is signalled by the pending-restart bookkeeping */
                handle->stats.bus_state = handle->recovery.restart_pending
                                          ? CAN_CORE_BUS_RECOVERING
                                          : CAN_CORE_BUS_RUNNING;
                break;
        }
    }
#else
    handle->stats.bus_state = handle->initialised ? CAN_CORE_BUS_RUNNING
                                                  : CAN_CORE_BUS_STOPPED;
#endif

    *stats = handle->stats;
}

/* -------------------------------------------------------------------------
 * can_core_reset_stats
 * ------------------------------------------------------------------------- */
void can_core_reset_stats(can_core_handle_t *handle)
{
    if (!handle)
    {
        return;
    }
    memset(&handle->stats, 0, sizeof(handle->stats));
}

elm327_err_t can_core_set_silent_mode(can_core_handle_t *handle,
                                        bool silent_mode)
{
    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    if (handle->config.silent_mode == silent_mode)
    {
        return ELM327_OK;
    }

    handle->config.silent_mode = silent_mode;

#ifdef ESP_PLATFORM
    handle->reconfiguring = true;
    can_rx_wait_parked(handle);
    can_driver_stop(handle);
    elm327_err_t err = can_driver_start_on_core(handle, &handle->config);
    handle->reconfiguring = false;
    return err;
#else
    return ELM327_OK;
#endif
}

/* -------------------------------------------------------------------------
 * can_core_set_baud
 * ------------------------------------------------------------------------- */
elm327_err_t can_core_set_baud(can_core_handle_t *handle,
                                  uint32_t baud_kbps)
{
    if (!handle || !handle->initialised)
    {
        return ELM327_ERR_NOT_INIT;
    }

    handle->config.baud_kbps = baud_kbps;

#ifdef ESP_PLATFORM
    handle->reconfiguring = true;
    can_rx_wait_parked(handle);
    can_driver_stop(handle);
    elm327_err_t err = can_driver_start_on_core(handle, &handle->config);
    handle->reconfiguring = false;
    return err;
#else
    return ELM327_OK;
#endif
}

static bool can_subscription_matches_frame(uint32_t filter,
                                           uint32_t mask,
                                           bool ext,
                                           bool monitor_all,
                                           const can_core_frame_t *frame)
{
    if (!frame)
    {
        return false;
    }

    if (monitor_all)
    {
        return true;
    }

    if (frame->ext != ext)
    {
        return false;
    }

    return can_core_filter_match(frame->id, filter, mask);
}

/** @return false when the queue was full and the oldest frame was
 *          evicted to make room (the caller counts drops). */
static bool dispatch_frame_to_queue_subscriber(QueueHandle_t queue,
                                               const can_core_frame_t *frame)
{
#ifdef ESP_PLATFORM
    can_core_frame_t dropped_frame;

    if (queue == NULL || frame == NULL)
    {
        return true;
    }

    if (xQueueSendToBack(queue, frame, 0) == pdPASS)
    {
        return true;
    }

    (void)xQueueReceive(queue, &dropped_frame, 0);
    (void)xQueueSendToBack(queue, frame, 0);
    return false;
#else
    (void)queue;
    (void)frame;
    return true;
#endif
}

#ifdef ESP_PLATFORM
static elm327_err_t can_driver_start(can_core_handle_t *handle,
                                     const can_core_config_t *config)
{
    if (config->baud_kbps < 25 || config->baud_kbps > 1000)
    {
        ESP_LOGE(TAG, "Unsupported baud rate: %lu kbps",
                 config->baud_kbps);
        return ELM327_ERR_CAN;
    }

    if (s_rx_q == NULL)
    {
        s_rx_q = xQueueCreateStatic(CAN_CORE_RXQ_DEPTH,
                                    sizeof(can_rx_raw_t), s_rxq_store,
                                    &s_rxq_buf);
    }
    xQueueReset(s_rx_q);
    s_evt_bus_off = false;
    s_evt_recovered = false;

    twai_onchip_node_config_t node_cfg =
    {
        .io_cfg =
        {
            .tx = (gpio_num_t)config->tx_gpio,
            .rx = (gpio_num_t)config->rx_gpio,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = { .bitrate = config->baud_kbps * 1000 },
        .tx_queue_depth = (uint32_t)(config->tx_queue_depth > 0
                                     ? config->tx_queue_depth : 16),
        /* FINITE retry (was -1 = forever): a tx on a mismatched-baud
           bus parks the node error-passive (TEC pinned at 128, never
           bus-off) spewing ~1800 error frames/s UNTIL the next driver
           bounce — the reconfig hammer surfaced it 2026-07-22. 512
           attempts ≈ 130 ms at 500k: rides out any real arbitration/
           error transient, then gives up like an ELM327 does. */
        .fail_retry_cnt = 512,
        .flags = { .enable_listen_only = config->silent_mode },
    };

    can_core_recovery_reset(&handle->recovery);

    esp_err_t ret = twai_new_node_onchip(&node_cfg, &s_node);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "twai_new_node_onchip failed: %d", ret);
        s_node = NULL;
        return ELM327_ERR_CAN;
    }

    twai_event_callbacks_t cbs =
    {
        .on_rx_done = can_on_rx_done,
        .on_state_change = can_on_state_change,
    };

    ret = twai_node_register_event_callbacks(s_node, &cbs, handle);

    if (ret == ESP_OK)
    {
        ret = twai_node_enable(s_node);
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "twai node enable failed: %d", ret);
        (void)twai_node_delete(s_node);
        s_node = NULL;
        return ELM327_ERR_CAN;
    }

    return ELM327_OK;
}

/* Rendezvous with the RX task before a driver bounce: caller sets
 * handle->reconfiguring, then waits for rx_parked -- the task must be
 * provably outside a receive when the node is torn down (belt to the
 * static-queue braces; kept from the 2026-07-21 legacy-driver fix). */
static void can_rx_wait_parked(can_core_handle_t *handle)
{
    for (int i = 0; i < 50 && !handle->rx_parked && !handle->rx_exited;
         i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!handle->rx_parked && !handle->rx_exited)
    {
        ESP_LOGE(TAG, "rx task did not park; reconfiguring anyway");
    }
}

static void can_driver_stop(can_core_handle_t *handle)
{
    if (!handle || s_node == NULL)
    {
        return;
    }

    (void)twai_node_disable(s_node);

    esp_err_t ret = twai_node_delete(s_node);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "twai_node_delete failed: %d", ret);
    }

    s_node = NULL;
}
#endif
