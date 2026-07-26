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
 * @file usb_cdc_device.c
 * @brief CDC-ACM device (virtual COM port) carrying the J2534 wire protocol
 *        over serial. See include/usb_cdc_device.h.
 *
 * Context rules mirror usb_net_device: CherryUSB endpoint/class callbacks
 * run in the USB INTERRUPT on the ESP port — they only move bytes to/from
 * a StreamBuffer and flip flags. A PSRAM-stacked session task owns the
 * blocking j2534_server_serve_serial() loop; the server's read/write go
 * through the transport vtable registered here.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#include "j2534_server.h"
#include "usb_cdc_device.h"

static const char *TAG = "usb_cdc_device";

#define UCD_EP_INT 0x83
#define UCD_EP_OUT 0x02
#define UCD_EP_IN  0x81
#define UCD_MPS    64

#define UCD_RX_DMA_CAP   512
#define UCD_TX_DMA_CAP   (J2534_MAX_DATA_HINT)  /* one frame; see below */
#define UCD_RX_SB_CAP    4096   /* StreamBuffer between the ISR and read() */

/* A full wire frame is a 12-byte header + up to J2534_MAX_DATA payload.
 * j2534_server.h doesn't export the max, so size the TX DMA buffer for a
 * comfortable ceiling (a PASSTHRU_MSG + header); large multi-frame ISO-TP
 * is chunked by the caller. */
#ifndef J2534_MAX_DATA_HINT
#define J2534_MAX_DATA_HINT 4160
#endif

static usb_cdc_device_status_t s_st;
static bool s_started;
static char s_serial[13];

static StreamBufferHandle_t s_rx_sb;      /* ISR bulk-out -> read()      */
static SemaphoreHandle_t s_tx_done;       /* bulk-in completion          */
static uint8_t *s_rx_dma;                 /* INTERNAL|DMA                */
static uint8_t *s_tx_dma;                 /* INTERNAL|DMA                */

static volatile bool s_configured;
static volatile bool s_port_open;         /* host asserted DTR           */

static TaskHandle_t s_session_task;
static StaticTask_t s_session_tcb;                       /* internal     */
static StackType_t s_session_stack[4096] EXT_RAM_BSS_ATTR;

static struct usbd_interface s_intf_comm;
static struct usbd_interface s_intf_data;
static struct usbd_endpoint s_ep_out;
static struct usbd_endpoint s_ep_in;

/* ---- descriptors ----------------------------------------------------------- */

static const uint8_t s_device_desc[] =
{
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               CONFIG_WICAN_USB_DEV_VID,
                               CONFIG_WICAN_USB_DEV_PID_CDC, 0x0100, 0x01),
};

static const uint8_t s_config_desc[] =
{
    USB_CONFIG_DESCRIPTOR_INIT((9 + CDC_ACM_DESCRIPTOR_LEN), 0x02, 0x01,
                               USB_CONFIG_BUS_POWERED, 250),
    CDC_ACM_DESCRIPTOR_INIT(0x00, UCD_EP_INT, UCD_EP_OUT, UCD_EP_IN,
                            UCD_MPS, 0x00),
};

static const uint8_t *ucd_device_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return s_device_desc;
}

static const uint8_t *ucd_config_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return s_config_desc;
}

static const uint8_t *ucd_device_quality_descriptor_cb(uint8_t speed)
{
    (void)speed;
    return NULL;
}

static const char *ucd_string_descriptor_cb(uint8_t speed, uint8_t index)
{
    (void)speed;

    switch (index)
    {
        case 0:  return "\x09\x04"; /* langid 0x0409 */
        case 1:  return "MeatPi Electronics";
        case 2:  return "WiCAN Pro J2534";
        case 3:  return s_serial;
        default: return NULL;
    }
}

static const struct usb_descriptor s_usbd_descriptor =
{
    .device_descriptor_callback = ucd_device_descriptor_cb,
    .config_descriptor_callback = ucd_config_descriptor_cb,
    .device_quality_descriptor_callback = ucd_device_quality_descriptor_cb,
    .string_descriptor_callback = ucd_string_descriptor_cb,
};

/* ---- ISR-side USB callbacks ------------------------------------------------ */

static void ucd_arm_read(void)
{
    usbd_ep_start_read(0, UCD_EP_OUT, s_rx_dma, UCD_RX_DMA_CAP);
}

static void ucd_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    BaseType_t woken = pdFALSE;

    (void)busid;
    (void)ep;

    if (nbytes > 0)
    {
        s_st.rx_bytes += nbytes;
        /* drop bytes that don't fit rather than block the ISR */
        (void)xStreamBufferSendFromISR(s_rx_sb, s_rx_dma, nbytes, &woken);
    }

    ucd_arm_read();
    portYIELD_FROM_ISR(woken);
}

static void ucd_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    BaseType_t woken = pdFALSE;

    (void)busid;

    if (nbytes > 0 && (nbytes % usbd_get_ep_mps(0, ep)) == 0)
    {
        usbd_ep_start_write(0, ep, NULL, 0); /* ZLP to end the transfer */
        return;
    }

    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

static void ucd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event)
    {
        case USBD_EVENT_CONFIGURED:
            s_configured = true;
            s_st.configured = true;
            ucd_arm_read();
            xTaskNotifyGive(s_session_task);
            break;
        case USBD_EVENT_RESET:
            s_configured = false;
            s_port_open = false;
            s_st.configured = false;
            s_st.port_open = false;
            break;
        default:
            break;
    }
}

/* host opened/closed the port (DTR). Gate the session on it so we don't
 * serve into a port nobody's reading. */
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;

    s_port_open = dtr;
    s_st.port_open = dtr;

    if (dtr && s_session_task != NULL)
    {
        xTaskNotifyGive(s_session_task);
    }
}

/* ---- j2534_server serial transport vtable ---------------------------------- */

static int ucd_read(uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    if (!s_configured || !s_port_open)
    {
        return -1; /* link down -> serve_client exits the session */
    }

    size_t got = xStreamBufferReceive(s_rx_sb, buf, n,
                                      pdMS_TO_TICKS(timeout_ms));
    return (int)got; /* 0 = timeout (caller retries) */
}

static int ucd_write(const uint8_t *buf, size_t n)
{
    if (!s_configured || !s_port_open)
    {
        return -1;
    }

    if (n > UCD_TX_DMA_CAP)
    {
        return -1;
    }

    /* one writer at a time (j2534_server holds its send-lock across the
     * header+payload writes), so a single DMA buffer + completion sem is
     * safe. Wait for any prior transfer, then start this one. */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return -1;
    }

    memcpy(s_tx_dma, buf, n);

    if (usbd_ep_start_write(0, UCD_EP_IN, s_tx_dma, n) < 0)
    {
        xSemaphoreGive(s_tx_done);
        return -1;
    }

    /* wait for the bulk-in completion (given in ucd_bulk_in) */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return -1; /* leave the sem taken; a reset/close recovers state */
    }

    xSemaphoreGive(s_tx_done); /* ready for the next write */
    s_st.tx_bytes += n;
    return (int)n;
}

static const j2534_serial_transport_t s_transport =
{
    .read = ucd_read,
    .write = ucd_write,
};

/* ---- session task ---------------------------------------------------------- */

static void ucd_session_task(void *arg)
{
    (void)arg;

    while (true)
    {
        /* wait until the host has configured the device AND opened the
         * port (DTR). ulTaskNotifyTake wakes on configure/DTR events. */
        if (!s_configured || !s_port_open)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }

        /* flush any stale RX from a previous session before serving */
        xStreamBufferReset(s_rx_sb);
        ESP_LOGI(TAG, "port open — serving J2534 over CDC-ACM");
        j2534_server_serve_serial();     /* blocks for the session */
        ESP_LOGI(TAG, "serial session ended");
        /* serve_serial returns instantly if the server is disabled or a
         * TCP tester holds the session — throttle so we don't spin */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---- lifecycle ------------------------------------------------------------- */

esp_err_t usb_cdc_device_start(void)
{
    uint8_t mac[6];

    if (s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_rx_dma = heap_caps_malloc(UCD_RX_DMA_CAP,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_tx_dma = heap_caps_malloc(UCD_TX_DMA_CAP,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_rx_sb = xStreamBufferCreate(UCD_RX_SB_CAP, 1);
    s_tx_done = xSemaphoreCreateBinary();

    if (s_rx_dma == NULL || s_tx_dma == NULL || s_rx_sb == NULL ||
        s_tx_done == NULL)
    {
        ESP_LOGE(TAG, "alloc failed (internal DMA / stream buffer)");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_tx_done); /* nothing in flight */

    s_session_task = xTaskCreateStatic(ucd_session_task, "j2534_cdc",
                                       sizeof(s_session_stack) /
                                       sizeof(s_session_stack[0]),
                                       NULL, 6, s_session_stack,
                                       &s_session_tcb);
    if (s_session_task == NULL)
    {
        return ESP_FAIL;
    }

    j2534_server_set_serial_transport(&s_transport);

    /* register the CDC-ACM interfaces + bulk endpoints */
    usbd_desc_register(0, &s_usbd_descriptor);
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &s_intf_comm));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &s_intf_data));

    s_ep_out.ep_addr = UCD_EP_OUT;
    s_ep_out.ep_cb = ucd_bulk_out;
    s_ep_in.ep_addr = UCD_EP_IN;
    s_ep_in.ep_cb = ucd_bulk_in;
    usbd_add_endpoint(0, &s_ep_out);
    usbd_add_endpoint(0, &s_ep_in);

    if (usbd_initialize(0, ESP_USBD_BASE, ucd_event_handler) != 0)
    {
        ESP_LOGE(TAG, "usbd_initialize failed");
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "up: CDC-ACM J2534 serial device (vid 0x%04x pid 0x%04x) "
             "— waiting for the host", CONFIG_WICAN_USB_DEV_VID,
             CONFIG_WICAN_USB_DEV_PID_CDC);
    return ESP_OK;
}

esp_err_t usb_cdc_device_get_status(usb_cdc_device_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_st;
    return ESP_OK;
}
