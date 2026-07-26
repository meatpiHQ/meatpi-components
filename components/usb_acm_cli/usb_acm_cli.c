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
 * @file usb_acm_cli.c
 * @brief CDC-ACM host glue + RX task + the request/response console.
 *
 * CherryUSB calls usbh_cdc_acm_run/stop (WEAK, overridden here) when an
 * ACM interface enumerates/detaches. On run: store the instance, assert
 * DTR/RTS, start the RX task. The RX task reads bulk-in into a DMA-safe
 * internal buffer and pushes bytes to a stream buffer (for the
 * request/response path) AND fans chunks to bridge subscribers (raw
 * passthrough). Writes go bulk-out from a DMA-safe internal buffer
 * (the caller's data may be in PSRAM).
 *
 * Settings live in usb_acm_cli_settings.c (standard §4.1).
 */
#include "usb_acm_cli.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "usbh_core.h"
#include "usbh_cdc_acm.h"

#include "bridge_manager.h"
#include "log_manager.h"

#include "usb_acm_cli_private.h"

static const char *TAG = "usb_acm_cli";

#define ACM_DMA_CHUNK   64      /* CDC-ACM bulk MPS on FS is 64 */
#define ACM_RX_SB_SIZE  2048
#define ACM_RX_STACK    4096
#define ACM_RX_PRIO     5
#define ACM_MAX_SUBS    2

/* Request/response termination: the ESPNetLink console ends every
 * response with its prompt — that is the END marker. Quiet time is
 * only a FALLBACK for prompt-less output, and it must be LAZY: the
 * modem legs go silent mid-response (`lte -j` pauses >700 ms between
 * "iccid": and the value while it runs the AT query), not just
 * between echo and payload. The original 150 ms quiet-settle raced
 * even `ver`'s echo→body gap — the web-UI "no reply" bug. */
#define ACM_PROMPT            "esp>"
#define ACM_QUIET_FALLBACK_MS 2000

/* GPS poll (legacy usb_host cadence): the dongle's `gps -p -j` every 5 s
 * when connected → a parsed cache read by /api/gps, the web UI, and the
 * autopid GPS publisher (via the sink). A valid fix older than
 * GPS_MAX_AGE_MS is reported as no-fix (the poll went silent). */
#define ACM_GPS_POLL_MS   5000
#define ACM_GPS_MAX_AGE_MS 20000
#define ACM_GPS_STACK     4096
#define ACM_GPS_PRIO      3

/* ---- state ---------------------------------------------------------------- */

static bool s_started;

static struct usbh_cdc_acm *volatile s_acm;   /* the bound instance */

static StreamBufferHandle_t s_rx_sb;
static StaticStreamBuffer_t s_rx_sb_buf;
static uint8_t s_rx_sb_store[ACM_RX_SB_SIZE + 1];

static SemaphoreHandle_t s_tx_lock;
static StaticSemaphore_t s_tx_lock_buf;
static SemaphoreHandle_t s_cmd_lock;
static StaticSemaphore_t s_cmd_lock_buf;

/* DMA-capable internal buffers (bulk transfers can't use PSRAM) */
static uint8_t *s_rx_dma;
static uint8_t *s_tx_dma;

static TaskHandle_t s_rx_task;
static StaticTask_t s_rx_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_rx_stack[ACM_RX_STACK];
static volatile bool s_rx_run;

/* bridge subscribers (raw passthrough) */
static QueueHandle_t s_subs[ACM_MAX_SUBS];
static SemaphoreHandle_t s_subs_lock;
static StaticSemaphore_t s_subs_lock_buf;

/* GPS poll cache (PSRAM-stacked poll task; USB + parse only, no flash) */
static usb_acm_gps_t s_gps;
static uint32_t s_gps_stamp_ms;
static SemaphoreHandle_t s_gps_lock;
static StaticSemaphore_t s_gps_lock_buf;
static usb_acm_gps_sink_t s_gps_sink;
static TaskHandle_t s_gps_task;
static StaticTask_t s_gps_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_gps_stack[ACM_GPS_STACK];
static volatile bool s_gps_run;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- write ---------------------------------------------------------------- */

static esp_err_t acm_write(const uint8_t *data, size_t len)
{
    struct usbh_cdc_acm *acm = s_acm;

    if (acm == NULL || s_tx_dma == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);

    size_t sent = 0;
    esp_err_t err = ESP_OK;

    while (sent < len)
    {
        size_t chunk = len - sent;

        if (chunk > ACM_DMA_CHUNK)
        {
            chunk = ACM_DMA_CHUNK;
        }

        memcpy(s_tx_dma, data + sent, chunk); /* PSRAM → internal DMA */

        int r = usbh_cdc_acm_bulk_out_transfer(acm, s_tx_dma,
                                               (uint32_t)chunk, 3000);

        if (r <= 0)
        {
            err = ESP_FAIL;
            break;
        }

        sent += (size_t)r;
    }

    xSemaphoreGive(s_tx_lock);
    return err;
}

/* ---- RX task -------------------------------------------------------------- */

static void fan_to_subs(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    for (int i = 0; i < ACM_MAX_SUBS; i++)
    {
        if (s_subs[i] == NULL)
        {
            continue;
        }

        size_t off = 0;

        while (off < len)
        {
            bridge_chunk_t c;
            size_t take = len - off;

            if (take > sizeof(c.data))
            {
                take = sizeof(c.data);
            }

            c.len = (uint16_t)take;
            memcpy(c.data, data + off, take);
            off += take;
            (void)xQueueSend(s_subs[i], &c, 0); /* drop on full */
        }
    }

    xSemaphoreGive(s_subs_lock);
}

static void rx_task(void *arg)
{
    (void)arg;

    while (s_rx_run)
    {
        struct usbh_cdc_acm *acm = s_acm;

        if (acm == NULL || s_rx_dma == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int r = usbh_cdc_acm_bulk_in_transfer(acm, s_rx_dma, ACM_DMA_CHUNK,
                                              200);

        if (r > 0)
        {
            /* request/response path: bounded stream buffer */
            (void)xStreamBufferSend(s_rx_sb, s_rx_dma, (size_t)r, 0);
            /* passthrough path: fan to bridge subscribers */
            fan_to_subs(s_rx_dma, (size_t)r);
        }
        else if (r < 0 && r != -USB_ERR_TIMEOUT)
        {
            vTaskDelay(pdMS_TO_TICKS(50)); /* transfer error; back off */
        }
    }

    s_rx_task = NULL;
    vTaskDelete(NULL);
}

/* ---- CherryUSB class callbacks (attach/detach) ----------------------------- */

void usbh_cdc_acm_run(struct usbh_cdc_acm *cdc_acm_class)
{
    /* no xStreamBufferReset here: the RX task may be mid-send (it is the
     * buffer's one writer) — usb_acm_cli_command() drains stale bytes
     * reader-side before every request instead */
    s_acm = cdc_acm_class;
    (void)usbh_cdc_acm_set_line_state(cdc_acm_class, true, true); /* DTR+RTS */

    if (s_rx_task == NULL && usb_acm_cli_settings_enabled())
    {
        s_rx_run = true;
        s_rx_task = xTaskCreateStatic(rx_task, "usb_acm_rx", ACM_RX_STACK,
                                      NULL, ACM_RX_PRIO, s_rx_stack,
                                      &s_rx_tcb);
    }

    ESP_LOGI(TAG, "CDC-ACM console attached (intf %u)",
             cdc_acm_class->intf);
}

void usbh_cdc_acm_stop(struct usbh_cdc_acm *cdc_acm_class)
{
    if (s_acm == cdc_acm_class)
    {
        s_acm = NULL;
        ESP_LOGI(TAG, "CDC-ACM console detached");
    }
}

/* ---- public API ------------------------------------------------------------ */

bool usb_acm_cli_connected(void)
{
    return s_acm != NULL;
}

esp_err_t usb_acm_cli_command(const char *line, char *resp, size_t resp_cap,
                              size_t *resp_len, uint32_t timeout_ms)
{
    if (line == NULL || resp == NULL || resp_len == NULL || resp_cap == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_acm == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cmd_lock, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    /* Flush stale RX by DRAINING, never xStreamBufferReset: the RX task
     * is a concurrent WRITER, and reset is not safe against an in-flight
     * send (stream buffers support exactly one reader + one writer — and
     * this command path is the one reader). resp doubles as scratch. */
    while (xStreamBufferReceive(s_rx_sb, resp, resp_cap - 1, 0) > 0)
    {
        /* discard */
    }

    size_t n = strlen(line);
    esp_err_t err = acm_write((const uint8_t *)line, n);

    if (err == ESP_OK && (n == 0 || line[n - 1] != '\r'))
    {
        err = acm_write((const uint8_t *)"\r", 1);
    }

    if (err != ESP_OK)
    {
        xSemaphoreGive(s_cmd_lock);
        return err;
    }

    size_t got = 0;
    uint32_t waited = 0;
    uint32_t quiet = 0;

    while (waited < timeout_ms && got + 1 < resp_cap)
    {
        size_t r = xStreamBufferReceive(s_rx_sb, resp + got,
                                        resp_cap - 1 - got,
                                        pdMS_TO_TICKS(50));
        waited += 50;

        if (r > 0)
        {
            got += r;
            quiet = 0;

            /* trailing whitespace then the prompt = response complete */
            size_t end = got;

            while (end > 0 && (resp[end - 1] == '\r' ||
                               resp[end - 1] == '\n' ||
                               resp[end - 1] == ' '))
            {
                end--;
            }

            const size_t pn = sizeof(ACM_PROMPT) - 1;

            if (end >= pn &&
                memcmp(resp + end - pn, ACM_PROMPT, pn) == 0)
            {
                break;
            }
        }
        else
        {
            quiet += 50;

            if (got > 0 && quiet >= ACM_QUIET_FALLBACK_MS)
            {
                break; /* prompt-less output settled */
            }
        }
    }

    resp[got] = '\0';
    *resp_len = got;
    xSemaphoreGive(s_cmd_lock);
    return ESP_OK;
}

/* ---- bridge endpoint (raw passthrough) ------------------------------------- */

static esp_err_t ep_send(const uint8_t *data, size_t len)
{
    if (s_acm == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return acm_write(data, len);
}

static esp_err_t ep_subscribe(QueueHandle_t q)
{
    esp_err_t err = ESP_ERR_NO_MEM;

    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    for (int i = 0; i < ACM_MAX_SUBS; i++)
    {
        if (s_subs[i] == NULL)
        {
            s_subs[i] = q;
            err = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_subs_lock);
    return err;
}

static esp_err_t ep_unsubscribe(QueueHandle_t q)
{
    xSemaphoreTake(s_subs_lock, portMAX_DELAY);

    for (int i = 0; i < ACM_MAX_SUBS; i++)
    {
        if (s_subs[i] == q)
        {
            s_subs[i] = NULL;
        }
    }

    xSemaphoreGive(s_subs_lock);
    return ESP_OK;
}

/* ---- GPS poll + cache ------------------------------------------------------- */

void usb_acm_cli_set_gps_sink(usb_acm_gps_sink_t sink)
{
    s_gps_sink = sink;
}

esp_err_t usb_acm_cli_gps_get(usb_acm_gps_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_gps_lock, portMAX_DELAY);
    *out = s_gps;
    uint32_t age = now_ms() - s_gps_stamp_ms;
    xSemaphoreGive(s_gps_lock);

    /* a fix the poll stopped refreshing (dongle gone quiet) is stale */
    if (out->valid && (!usb_acm_cli_connected() || age > ACM_GPS_MAX_AGE_MS))
    {
        memset(out, 0, sizeof(*out));
    }
    else
    {
        out->age_ms = out->valid ? age : 0;
    }

    return ESP_OK;
}

static void gps_task(void *arg)
{
    (void)arg;
    static char resp[768]; /* lte -j is the biggest; gps -p -j ~410 B */

    /* the task never exits — stop() just idles it (s_gps_run false), so
     * a stop()/start() cycle can't lose the poll to a teardown race */
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(ACM_GPS_POLL_MS));

        if (!s_gps_run || !usb_acm_cli_connected())
        {
            continue;
        }

        size_t rn = 0;

        if (usb_acm_cli_command("gps -p -j", resp, sizeof(resp), &rn,
                                3000) != ESP_OK)
        {
            continue; /* console busy this round — try again next tick */
        }

        usb_acm_gps_t fix;

        if (!usb_acm_gps_parse(resp, &fix))
        {
            /* no live fix — mark the cache invalid so readers see no-fix */
            xSemaphoreTake(s_gps_lock, portMAX_DELAY);
            s_gps.valid = false;
            s_gps_stamp_ms = now_ms();
            xSemaphoreGive(s_gps_lock);
            continue;
        }

        xSemaphoreTake(s_gps_lock, portMAX_DELAY);
        s_gps = fix;
        s_gps_stamp_ms = now_ms();
        xSemaphoreGive(s_gps_lock);

        if (s_gps_sink != NULL)
        {
            s_gps_sink(&fix); /* → autopid publisher (main-wired) */
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t usb_acm_cli_init(void)
{
    static const log_descriptor_t LOG_DESC = { "usb_acm_cli", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    s_rx_sb = xStreamBufferCreateStatic(ACM_RX_SB_SIZE, 1, s_rx_sb_store,
                                        &s_rx_sb_buf);
    s_tx_lock = xSemaphoreCreateMutexStatic(&s_tx_lock_buf);
    s_cmd_lock = xSemaphoreCreateMutexStatic(&s_cmd_lock_buf);
    s_subs_lock = xSemaphoreCreateMutexStatic(&s_subs_lock_buf);
    s_gps_lock = xSemaphoreCreateMutexStatic(&s_gps_lock_buf);

    return usb_acm_cli_settings_register();
}

esp_err_t usb_acm_cli_start(void)
{
    if (!usb_acm_cli_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    if (!usb_acm_cli_settings_enabled())
    {
        ESP_LOGI(TAG, "disabled by settings");
        return ESP_OK;
    }

    /* DMA-capable internal buffers for the bulk transfers */
    s_rx_dma = heap_caps_malloc(ACM_DMA_CHUNK,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_tx_dma = heap_caps_malloc(ACM_DMA_CHUNK,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (s_rx_dma == NULL || s_tx_dma == NULL)
    {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* raw passthrough endpoint (like obd) */
    static const bridge_endpoint_t EP =
    {
        .name = "acm", .send = ep_send,
        .subscribe = ep_subscribe, .unsubscribe = ep_unsubscribe,
    };

    (void)bridge_manager_register_endpoint(&EP);

    /* GPS poll task: caches the dongle's fix + feeds the autopid publisher.
     * PSRAM stack — it only does USB + a pure parse (no flash). */
    if (s_gps_task == NULL)
    {
        s_gps_run = true;
        s_gps_task = xTaskCreateStatic(gps_task, "usb_acm_gps",
                                       ACM_GPS_STACK, NULL, ACM_GPS_PRIO,
                                       s_gps_stack, &s_gps_tcb);
    }

    ESP_LOGI(TAG, "started (CDC-ACM console; endpoint 'acm'; GPS poll)");
    return ESP_OK;
}

esp_err_t usb_acm_cli_stop(void)
{
    s_rx_run = false;
    s_gps_run = false;
    s_started = false;
    return ESP_OK;
}
