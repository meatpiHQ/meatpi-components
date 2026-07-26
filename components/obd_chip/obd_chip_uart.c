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
 * @brief UART1 + control pins: driver install, serialized TX, the RX fan-out
 *        task, and the sleep/wake/reset pin sequences (from meatpi — the
 *        sleep hold SURVIVES resets, so waking requires the release
 *        sequence, verified on the bench 2026-07-03).
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "obd_chip.h"
#include "obd_chip_private.h"

static const char *TAG = "obd_chip";

#define OBD_UART      UART_NUM_1
#define OBD_TX_PIN    GPIO_NUM_16
#define OBD_RX_PIN    GPIO_NUM_15
#define OBD_READY_PIN GPIO_NUM_7  /* chip-driven; LOW = awake/ready, HIGH =
                                     asleep (legacy elm327_chip_get_status:
                                     ELM327_READY==0 — task §11.1 answered) */
#define OBD_SLEEP_PIN GPIO_NUM_9  /* manager-driven; high = awake            */
#define OBD_RESET_PIN GPIO_NUM_41 /* active-low pulse; OPEN-DRAIN output
                                     (legacy init — never drive it high)     */

/* The chip can burst (own 8 KB buffer) at 2 Mbaud — legacy sizes. The
 * driver allocates these ring buffers itself with MALLOC_CAP_DEFAULT
 * (UART_ISR_IN_IRAM off), which our SPIRAM malloc policy places in
 * PSRAM — same as legacy; no DMA involved (the ISR copies the 128 B
 * hardware FIFO into the ring). Only the small driver object and FIFO
 * stash live internal. */
#define OBD_UART_RX_BUF (18 * 1024)
#define OBD_UART_TX_BUF (18 * 1024)

static SemaphoreHandle_t s_tx_lock;
static StaticSemaphore_t s_tx_lock_buf; /* internal: FreeRTOS object */

static TaskHandle_t s_rx_task;
static StaticTask_t s_rx_tcb; /* internal: FreeRTOS object */
/* internal: UART driver path (legacy keeps these stacks internal; the
 * driver's ISR interaction is not PSRAM-safe territory) */
static StackType_t s_rx_stack[4096];
static volatile bool s_rx_running;

/* ---- pins -------------------------------------------------------------------- */

void obd_pins_init(void)
{
    /* byte-for-byte the legacy init block (meatpi) */
    gpio_reset_pin(OBD_RESET_PIN);
    gpio_set_direction(OBD_RESET_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(OBD_RESET_PIN, 1); /* reset is active low */

    gpio_reset_pin(OBD_READY_PIN);
    gpio_set_direction(OBD_READY_PIN, GPIO_MODE_INPUT);
    gpio_pulldown_en(OBD_READY_PIN);
}

void obd_pin_wake(void)
{
    /* the sleep path parks the pin low with pulldown + gpio_hold_en + RTC
       pulldown; holds survive resets — release everything first (meatpi) */
    gpio_sleep_set_pull_mode(OBD_SLEEP_PIN, GPIO_FLOATING);
    gpio_pulldown_en(OBD_SLEEP_PIN);
    rtc_gpio_pulldown_dis(OBD_SLEEP_PIN);
    gpio_hold_dis(OBD_SLEEP_PIN);
    rtc_gpio_deinit(OBD_SLEEP_PIN);
    gpio_reset_pin(OBD_SLEEP_PIN);
    gpio_set_direction(OBD_SLEEP_PIN, GPIO_MODE_OUTPUT);
    gpio_pulldown_en(OBD_SLEEP_PIN);
    gpio_set_level(OBD_SLEEP_PIN, 1);
}

void obd_pin_sleep(void)
{
    gpio_sleep_set_pull_mode(OBD_SLEEP_PIN, GPIO_PULLDOWN_ONLY);
    gpio_set_level(OBD_SLEEP_PIN, 0);
    gpio_pulldown_en(OBD_SLEEP_PIN);
    rtc_gpio_pulldown_en(OBD_SLEEP_PIN);
    gpio_hold_en(OBD_SLEEP_PIN); /* survives resets on purpose */

    /* legacy parks the READY input too (pulled low = "asleep" reading,
       no float current during deep sleep) */
    gpio_sleep_set_pull_mode(OBD_READY_PIN, GPIO_PULLDOWN_ONLY);
    rtc_gpio_pulldown_en(OBD_READY_PIN);
    gpio_pulldown_en(OBD_READY_PIN);
    gpio_hold_en(OBD_READY_PIN);
}

void obd_pin_reset_pulse(void)
{
    gpio_set_level(OBD_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(OBD_RESET_PIN, 1);
}

bool obd_pin_ready(void)
{
    /* ACTIVE LOW: legacy elm327_chip_get_status() maps level directly
       onto {ELM327_READY = 0, ELM327_SLEEP = 1} */
    return gpio_get_level(OBD_READY_PIN) == 0;
}

/* ---- UART --------------------------------------------------------------------- */

esp_err_t obd_uart_init(int baud)
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

    if (s_tx_lock == NULL)
    {
        s_tx_lock = xSemaphoreCreateMutexStatic(&s_tx_lock_buf);
    }

    esp_err_t err = uart_driver_install(OBD_UART, OBD_UART_RX_BUF,
                                        OBD_UART_TX_BUF, 0, NULL, 0);

    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_param_config(OBD_UART, &cfg);

    if (err != ESP_OK)
    {
        return err;
    }

    return uart_set_pin(OBD_UART, OBD_TX_PIN, OBD_RX_PIN,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

esp_err_t obd_uart_set_baud(int baud)
{
    return uart_set_baudrate(OBD_UART, baud);
}

esp_err_t obd_uart_write(const uint8_t *data, size_t len)
{
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    int written = uart_write_bytes(OBD_UART, data, len);
    esp_err_t err = (written == (int)len) ? ESP_OK : ESP_FAIL;

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

void obd_uart_flush_input(void)
{
    uart_flush_input(OBD_UART);
}

/* ---- RX fan-out task ------------------------------------------------------------- */

static void rx_task(void *arg)
{
    /* internal: handed to the UART driver read path */
    static uint8_t chunk[OBD_CHIP_CHUNK_SIZE];

    (void)arg;
    ESP_LOGD(TAG, "rx task up");

    while (s_rx_running)
    {
        /* MUST check BEFORE reading: during EXCLUSIVE the fw-update engine
           reads the UART itself — a read here would steal its responses */
        if (obd_core_exclusive_held())
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int got = uart_read_bytes(OBD_UART, chunk, sizeof(chunk),
                                  pdMS_TO_TICKS(20));

        if (got > 0)
        {
            obd_core_fanout(chunk, (size_t)got);
        }
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
