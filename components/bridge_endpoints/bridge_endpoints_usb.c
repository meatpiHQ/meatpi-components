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
 * @file bridge_endpoints_usb.c
 * @brief The `usb_obd` jack: WiCAN Pro's USB port-B UART (second CDC
 *        channel, UART2 TX=GPIO17/RX=GPIO18). The USB↔OBD transparent
 *        passthrough is a configured `raw` bridge over this jack
 *        (TASK_obd_chip_manager_new §2), enabled in settings.
 */
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bridge_manager.h"

#include "bridge_endpoints_private.h"

static const char *TAG = "bridge_endpoints";

#define USB_UART      UART_NUM_2
#define USB_TX_PIN    17
#define USB_RX_PIN    18
#define USB_BAUD      2000000 /* matches the OBD chip side (meatpi) —
                                 no bottleneck through the passthrough */

static QueueHandle_t s_usb_sub;
static uint32_t s_usb_drops;
static TaskHandle_t s_usb_task;
static StaticTask_t s_usb_tcb;        /* internal: FreeRTOS object       */
static StackType_t s_usb_stack[3072]; /* internal: UART driver path (§2) */

esp_err_t bep_usb_send(const uint8_t *d, size_t l)
{
    return (uart_write_bytes(USB_UART, d, l) == (int)l) ? ESP_OK : ESP_FAIL;
}

esp_err_t bep_usb_subscribe(QueueHandle_t q)
{
    if (s_usb_sub != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_usb_sub = q;
    return ESP_OK;
}

esp_err_t bep_usb_unsubscribe(QueueHandle_t q)
{
    if (s_usb_sub != q)
    {
        return ESP_ERR_NOT_FOUND;
    }

    s_usb_sub = NULL;
    return ESP_OK;
}

static void usb_rx_task(void *arg)
{
    static bridge_chunk_t chunk;

    (void)arg;

    while (true)
    {
        int got = uart_read_bytes(USB_UART, chunk.data, sizeof(chunk.data),
                                  pdMS_TO_TICKS(20));

        if (got <= 0 || s_usb_sub == NULL)
        {
            continue;
        }

        chunk.len = (uint16_t)got;

        if (xQueueSend(s_usb_sub, &chunk, 0) != pdTRUE)
        {
            s_usb_drops++; /* bounded queue: drop-and-count, never block */

            if ((s_usb_drops % 100) == 1)
            {
                ESP_LOGW(TAG, "usb_obd subscriber full (%lu drops)",
                         (unsigned long)s_usb_drops);
            }
        }
    }
}

esp_err_t bep_usb_start(void)
{
    uart_config_t cfg =
    {
        .baud_rate = USB_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(USB_UART, 4096, 4096, 0, NULL, 0);

    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_param_config(USB_UART, &cfg);

    if (err == ESP_OK)
    {
        err = uart_set_pin(USB_UART, USB_TX_PIN, USB_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    if (err != ESP_OK)
    {
        return err;
    }

    s_usb_task = xTaskCreateStatic(usb_rx_task, "usb_obd_rx",
                                   sizeof(s_usb_stack) /
                                       sizeof(s_usb_stack[0]),
                                   NULL, 10, s_usb_stack, &s_usb_tcb);
    return (s_usb_task != NULL) ? ESP_OK : ESP_FAIL;
}
