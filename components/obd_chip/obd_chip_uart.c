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
 * @file obd_chip_uart.c
 * @brief UART1: driver install, serialized TX, the RX fan-out task and the
 *        baud detection. The chip's control lines (READY, the sleep pin,
 *        the reset line) live in obd_chip_pins.c since 2026-11-12.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "log_manager.h"

#include "obd_chip.h"
#include "obd_chip_private.h"

static const char *TAG = "obd_chip";

#define OBD_UART      UART_NUM_1
#define OBD_TX_PIN    GPIO_NUM_16
#define OBD_RX_PIN    GPIO_NUM_15

/* The chip can burst (own 8 KB buffer) at 2 Mbaud: legacy sizes. The
 * driver allocates these ring buffers itself with MALLOC_CAP_DEFAULT
 * (UART_ISR_IN_IRAM off), which our SPIRAM malloc policy places in
 * PSRAM, same as legacy; no DMA involved (the ISR copies the 128 B
 * hardware FIFO into the ring). Only the small driver object and FIFO
 * stash live internal. */
#define OBD_UART_RX_BUF (18 * 1024)
#define OBD_UART_TX_BUF (18 * 1024)

/* The driver's event queue: the RX task blocks on it instead of polling
 * uart_read_bytes with a time budget. uart_read_bytes(128, 20 ms) kept
 * waiting for "more" after the LAST byte of every short response until
 * the budget ran out (bench 2026-09-08: a 12-byte hinted 010C answer cost
 * 23 ms end to end, the chip itself ~3 ms). The UART_DATA event fires at
 * the FIFO rx-timeout (10 symbol times = 50 us at 2 Mbaud), so a response
 * is fanned out as soon as the chip finished printing it - the legacy
 * firmware read on the same event (wc_uart.c / elm327.c). */
#define OBD_UART_EVT_DEPTH 32
#define OBD_UART_EVT_WAIT_MS 100 /* wake to re-check the running flag */

/* The hand-over of the hardware FIFO (128 bytes), two settings.
 *
 * The driver's ISR is called when OBD_UART_RX_FULL_THRESH bytes wait in the
 * FIFO, or at the rx-timeout for less. At 2 Mbaud a byte arrives every 5 us,
 * so the ISR has (128 - threshold) * 5 us to start reading before the FIFO
 * runs over. With the SDK's default of 120 that was 8 bytes, 40 us, and a
 * long output of the chip (ATPPS, STSLCS, the protocol tables: 250 to 1600
 * bytes unbroken) arrived cut: 40 of 280 on the bench of 2026-11-11, one in
 * four with the web server busy. (The bring-up's own STSLCS and ATPPS reads
 * go through the same FIFO; in 20 boots of that bench they were on time.)
 *
 * What made the ISR late was its place. The driver binds it to the core
 * that installs the driver, which was core 0, behind the WiFi interrupts
 * (and the Bluetooth, timer and bus ones) at the same level: of 11349
 * hand-overs there, the web server busy for half, 2.2 % started more than
 * 40 us late, and the latest of three runs 310 us. Installed from
 * OBD_UART_ISR_CORE, the app core, 0.07 % of 11514 did, and the latest of
 * three runs 200 us, once. (Level 3 on core 0 measured the same, 0.03 %,
 * but Bluetooth's controller takes its interrupts at level 3 there, and
 * that core has two level-triggered slots for it.) At a threshold of 32
 * the ISR has 96 bytes, 480 us: 2.4 times the latest seen on the app core.
 * Neither setting is enough alone: at 0.07 % a threshold of 120 would still
 * lose a hand-over now and then, and 32 on core 0 passed its reads with one
 * hand-over in 45 late and 170 us to spare, which is not a margin.
 *
 * Nothing covers a flash write: the ISR is flash code (UART_ISR_IN_IRAM is
 * off, its ring buffers would have to leave PSRAM) and does not run while
 * the cache is off, so a long output under one still loses bytes.
 *
 * What the fan-out hands on does not change with the threshold: a hand-over
 * that is not the end of a burst only passes on whole chunks (rx_drain), and
 * the always-timeout mode makes the rx-timeout the end of EVERY burst (the
 * ISR leaves one byte in the FIFO at a hand-over, so a burst that stops on
 * a multiple of the threshold still times out). */
#define OBD_UART_RX_FULL_THRESH 32
#define OBD_UART_LATE_BYTES     8 /* 40 us: all the old threshold allowed  */
#define OBD_UART_TAIL_WAIT_MS   5 /* a burst's end event lost: drain anyway */
#define OBD_UART_ISR_CORE       (portNUM_PROCESSORS - 1) /* the app core */

_Static_assert(OBD_UART_RX_FULL_THRESH < SOC_UART_FIFO_LEN,
               "the hand-over point lies inside the hardware FIFO");

static QueueHandle_t s_evt_q; /* driver-owned (uart_driver_install) */

/* wire accounting for GET /api/obd_chip */
static uint32_t s_tx_bytes;
static uint32_t s_rx_overflows;
static uint16_t s_rx_fifo_peak; /* the fullest an ISR pass found the FIFO */
static uint32_t s_rx_handovers; /* ISR passes at or above the threshold   */
static uint32_t s_rx_late;      /* of those, started OBD_UART_LATE_BYTES
                                   or more behind their interrupt         */

/* the baud the UART runs at now (the one set last): the firmware update
 * gate probes the chip here first, then at the other one (issue #1023) */
static int s_baud;

static SemaphoreHandle_t s_tx_lock;
static StaticSemaphore_t s_tx_lock_buf; /* internal: FreeRTOS object */

static TaskHandle_t s_rx_task;
static StaticTask_t s_rx_tcb; /* internal: FreeRTOS object */
/* internal: UART driver path (legacy keeps these stacks internal; the
 * driver's ISR interaction is not PSRAM-safe territory) */
static StackType_t s_rx_stack[4096];
static volatile bool s_rx_running;

/* ---- UART --------------------------------------------------------------------- */

/** The driver install and the port's configuration, on the calling core:
 *  the driver binds its ISR to the core that runs the install. */
static esp_err_t uart_install(int baud)
{
    uart_config_t cfg =
    {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(OBD_UART, OBD_UART_RX_BUF,
                                        OBD_UART_TX_BUF, OBD_UART_EVT_DEPTH,
                                        &s_evt_q, 0);

    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_param_config(OBD_UART, &cfg);

    if (err != ESP_OK)
    {
        return err;
    }

    /* the driver install left the hand-over point at the SDK's 120 */
    err = uart_set_rx_full_threshold(OBD_UART, OBD_UART_RX_FULL_THRESH);

    if (err != ESP_OK)
    {
        return err;
    }

    uart_set_always_rx_timeout(OBD_UART, true);
    s_baud = baud;
    return uart_set_pin(OBD_UART, OBD_TX_PIN, OBD_RX_PIN,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

typedef struct
{
    int               baud;
    esp_err_t         result;
    SemaphoreHandle_t done;
} uart_install_ctx_t;

static void uart_install_task(void *arg)
{
    uart_install_ctx_t *ctx = (uart_install_ctx_t *)arg;

    ctx->result = uart_install(ctx->baud);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

esp_err_t obd_uart_init(int baud)
{
    static StaticSemaphore_t done_buf; /* internal: FreeRTOS object */
    uart_install_ctx_t ctx =
    {
        .baud = baud,
        .result = ESP_FAIL,
        .done = xSemaphoreCreateBinaryStatic(&done_buf),
    };

    if (s_tx_lock == NULL)
    {
        s_tx_lock = xSemaphoreCreateMutexStatic(&s_tx_lock_buf);
    }

    /* the install on a one-shot task pinned to OBD_UART_ISR_CORE (the
       can_manager pattern); on this core when the task cannot be made */
    if (xTaskCreatePinnedToCore(uart_install_task, "obd_uart_init", 3072, &ctx,
                                10, NULL, OBD_UART_ISR_CORE) != pdPASS)
    {
        ESP_LOGW(TAG, "no task for the UART install: its interrupt stays on "
                      "core %d", (int)xPortGetCoreID());
        return uart_install(baud);
    }

    (void)xSemaphoreTake(ctx.done, portMAX_DELAY);
    return ctx.result;
}

esp_err_t obd_uart_set_baud(int baud)
{
    esp_err_t err = uart_set_baudrate(OBD_UART, baud);

    if (err == ESP_OK)
    {
        s_baud = baud;
    }

    return err;
}

int obd_uart_get_baud(void)
{
    return s_baud;
}

esp_err_t obd_uart_write(const uint8_t *data, size_t len)
{
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    int written = uart_write_bytes(OBD_UART, data, len);
    esp_err_t err = (written == (int)len) ? ESP_OK : ESP_FAIL;

    if (written > 0)
    {
        s_tx_bytes += (uint32_t)written;
    }

    if (err == ESP_OK)
    {
        err = uart_wait_tx_done(OBD_UART, pdMS_TO_TICKS(2000));
    }

    xSemaphoreGive(s_tx_lock);
    return err;
}

int obd_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(OBD_UART, buf, len, pdMS_TO_TICKS(timeout_ms));
}

/** What a driver event says about the hardware FIFO: how full the ISR found
 *  it (a hand-over that started late found more than the threshold in it)
 *  and whether it ran over. A hand-over that is not a burst's end leaves
 *  one byte behind for the rx-timeout (always-timeout mode), hence the 1.
 *  @return true for an overflow event (the FIFO or the ring). */
static bool evt_note(const uart_event_t *evt)
{
    if (evt->type == UART_FIFO_OVF)
    {
        s_rx_fifo_peak = SOC_UART_FIFO_LEN;
        return true;
    }

    if (evt->type == UART_DATA)
    {
        size_t fill = evt->size + (evt->timeout_flag ? 0u : 1u);

        if (fill > s_rx_fifo_peak)
        {
            s_rx_fifo_peak = (uint16_t)fill;
        }

        /* the peak is one event; the share of late hand-overs is what
           tells the ISR's place (2.2 % behind the WiFi interrupts on
           core 0, 0.07 % on the app core, bench 2026-11-11) */
        if (fill >= OBD_UART_RX_FULL_THRESH)
        {
            s_rx_handovers++;

            if (fill >= OBD_UART_RX_FULL_THRESH + OBD_UART_LATE_BYTES)
            {
                s_rx_late++;
            }
        }
    }

    return evt->type == UART_BUFFER_FULL;
}

/** Book the waiting events without acting on them: their bytes belong to a
 *  direct reader (the bring-up's bare probes), nobody else reads the queue. */
static void evt_book(void)
{
    uart_event_t evt;
    unsigned over = 0;

    while (xQueueReceive(s_evt_q, &evt, 0) == pdTRUE)
    {
        over += evt_note(&evt) ? 1u : 0u;
    }

    if (over > 0)
    {
        s_rx_overflows += over;
        ESP_LOGW(TAG, "rx overflow (%u) under a direct read: an answer of "
                      "the bring-up was cut", over);
    }
}

void obd_uart_flush_input(void)
{
    /* no RX task (the bring-up): each bare probe flushes first, so the
       events of the answer before it are booked here. Its long reads
       (STSLCS, ATPPS) count in the peak, and one the FIFO ran over under is
       counted and logged; until 2026-11-11 those events were thrown away
       unread. With the RX task up the events are its own. */
    if (s_rx_task == NULL && s_evt_q != NULL)
    {
        evt_book();
    }

    uart_flush_input(OBD_UART);
}

void obd_uart_get_stats(obd_uart_stats_t *out)
{
    size_t buffered = 0;

    uart_get_buffered_data_len(OBD_UART, &buffered);
    out->tx_bytes = s_tx_bytes;
    out->rx_overflows = s_rx_overflows;
    out->rx_buffered = (uint32_t)buffered;
    out->rx_fifo_peak = s_rx_fifo_peak;
    out->rx_handovers = s_rx_handovers;
    out->rx_late = s_rx_late;
}

/* ---- baud detection (issue #1023, Ali's question 2026-10-28) --------------------
 * The S3 UART measures the pulses on RX by itself (the ROM uses it to sync
 * with esptool at any speed): the shortest low and high pulse over a
 * window are one bit time. IDF 6.1 wraps it (uart_detect_bitrate_start /
 * _stop); the driver's formula is clk * 2 / (low + high). A chip that
 * talks on its own (a banner, a reboot loop) is measured without a word
 * from us; a quiet chip needs a stimulus it answers at the current baud. */

esp_err_t obd_chip_autobaud(const char *stimulus, uint32_t window_ms,
                            obd_chip_autobaud_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS) ||
        obd_chip_update_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = obd_core_claim(3 /* CLAIM_EXCLUSIVE */, 5000);

    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    memset(out, 0, sizeof(*out));
    uart_flush_input(OBD_UART);

    /* the port is acquired by the driver: no config, the detector is just
       switched on; reception keeps running meanwhile (flushed after) */
    err = uart_detect_bitrate_start(OBD_UART, NULL);

    if (err == ESP_OK)
    {
        if (stimulus != NULL && stimulus[0] != '\0')
        {
            (void)obd_uart_write((const uint8_t *)stimulus, strlen(stimulus));
        }

        vTaskDelay(pdMS_TO_TICKS(window_ms));

        uart_bitrate_res_t res;

        err = uart_detect_bitrate_stop(OBD_UART, false, &res);

        if (err == ESP_OK)
        {
            out->edges = res.edge_cnt;
            out->clk_hz = res.clk_freq_hz;
            out->low = res.low_period;
            out->high = res.high_period;
            out->pos = res.pos_period;
            out->neg = res.neg_period;

            if (res.edge_cnt > 0)
            {
                uint64_t clk2 = (uint64_t)res.clk_freq_hz * 2u;

                if (res.low_period + res.high_period > 0)
                {
                    out->baud_lh = (int)(clk2 / (res.low_period + res.high_period));
                }

                if (res.pos_period > 0)
                {
                    out->baud_pos = (int)(clk2 / res.pos_period);
                }

                if (res.neg_period > 0)
                {
                    out->baud_neg = (int)(clk2 / res.neg_period);
                }
            }
        }
    }

    uart_flush_input(OBD_UART); /* whatever the window collected */
    obd_core_release();
    return err;
}

/* ---- RX fan-out task ------------------------------------------------------------- */

/** Fan out what the driver has buffered, chunk by chunk. The bytes are
 *  already in the ring buffer, so the 1-tick budget only guards the
 *  driver's rx mutex: there is no waiting for "more" after the last byte.
 *  @param whole_only  the burst is still arriving (a FIFO hand-over, not
 *         its end): only whole chunks go out, so that a long output reaches
 *         the subscribers in chunks of OBD_CHIP_CHUNK_SIZE whatever the
 *         hand-over point is (their queues are counted in chunks)
 *  @return the bytes left in the ring for the next event */
static size_t rx_drain(bool whole_only)
{
    /* internal: handed to the UART driver read path */
    static uint8_t chunk[OBD_CHIP_CHUNK_SIZE];
    size_t avail = 0;

    uart_get_buffered_data_len(OBD_UART, &avail);

    /* re-checked per chunk: EXCLUSIVE may be claimed while we sleep in
       the event wait, and the wake-up event could be the fw-update
       engine's own first response */
    while (avail > 0 && s_rx_running && !obd_core_exclusive_held())
    {
        if (whole_only && avail < sizeof(chunk))
        {
            return avail;
        }

        size_t want = (avail > sizeof(chunk)) ? sizeof(chunk) : avail;
        int got = uart_read_bytes(OBD_UART, chunk, want, 1);

        if (got <= 0)
        {
            break;
        }

        obd_core_fanout(chunk, (size_t)got);
        avail -= (size_t)got;
    }

    return 0;
}

static void rx_task(void *arg)
{
    uart_event_t evt;
    size_t left = 0; /* a part chunk kept back: the burst goes on */

    (void)arg;
    ESP_LOGD(TAG, "rx task up");

    while (s_rx_running)
    {
        /* MUST check BEFORE reading: during EXCLUSIVE the fw-update engine
           reads the UART itself - a read here would steal its responses.
           Events posted meanwhile are stale by the time we resume; the
           drain below reads by buffered length, not by event size. */
        if (obd_core_exclusive_held())
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            left = 0;
            continue;
        }

        /* with a part chunk kept back the next event is due within a
           hand-over (160 us) or at the burst's end: a short wait, so that
           a lost end event costs OBD_UART_TAIL_WAIT_MS and not the idle one */
        uint32_t wait_ms = (left > 0) ? OBD_UART_TAIL_WAIT_MS
                                      : OBD_UART_EVT_WAIT_MS;
        bool have_evt = (xQueueReceive(s_evt_q, &evt, pdMS_TO_TICKS(wait_ms))
                         == pdTRUE);

        if (have_evt && evt_note(&evt))
        {
            /* the hardware FIFO ran over (the ISR came too late for a
               hand-over), or the chip out-ran the 18 KB ring (monitor flood
               with every subscriber stalled): drop the backlog, keep the
               stream sane */
            uart_flush_input(OBD_UART);
            xQueueReset(s_evt_q);
            s_rx_overflows++;
            left = 0;
            ESP_LOGW(TAG, "rx overflow (%s) - input flushed",
                     evt.type == UART_FIFO_OVF ? "fifo" : "ring");
            continue;
        }

        /* drain on data events AND on the idle wake: a lost event (queue
           full while exclusive) can never strand bytes in the ring for
           longer than the wait above. Everything goes out at a burst's end
           (the rx-timeout) and on a wake without an event; whole chunks
           only at a hand-over in the middle of one. */
        left = rx_drain(have_evt && evt.type == UART_DATA && !evt.timeout_flag);
    }

    s_rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t obd_uart_rx_task_start(void)
{
    if (s_rx_task != NULL)
    {
        return ESP_OK;
    }

    if (s_evt_q == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* driver not installed */
    }

    /* the bring-up's last bare probe left its events behind; the ring
       itself was flushed before each probe, so the queue is only read for
       what it says about the FIFO (obd_uart_flush_input) */
    evt_book();
    s_rx_running = true;
    s_rx_task = xTaskCreateStatic(rx_task, "obd_chip_rx",
                                  sizeof(s_rx_stack) / sizeof(s_rx_stack[0]),
                                  NULL, 12, s_rx_stack, &s_rx_tcb);
    return (s_rx_task != NULL) ? ESP_OK : ESP_FAIL;
}

void obd_uart_rx_task_stop(void)
{
    s_rx_running = false;

    for (int i = 0; i < 20 && s_rx_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
