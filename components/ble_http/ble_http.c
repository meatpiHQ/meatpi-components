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
 * @file ble_http.c
 * @brief IDF glue: lifecycle, the "http" ble_manager channel, the tunnel
 *        task and the esp_http_client implementation of the core's
 *        HTTP-side vtable (loopback to the device's own web server). The
 *        route handlers run on the httpd task's INTERNAL stack, so this
 *        task can live on PSRAM and never touches flash itself.
 */
#include "ble_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "http_server_manager.h"
#include "log_manager.h"

#include "ble_http_core.h"
#include "ble_http_private.h"

static const char *TAG = "ble_http";

#define BLEH_UUID_OUT     0xFFF3          /* notify: device -> app        */
#define BLEH_UUID_IN      0xFFF4          /* write:  app -> device        */
/* channel StreamBuffer (PSRAM): twice the 16 KB credit window, because
   while the task waits for a CREDIT indication's confirmation it is not
   reading, and the phone may still have a whole window in flight */
#define BLEH_RX_STORAGE   32768
#define BLEH_SLICE        512             /* one channel read             */
#define BLEH_TASK_STACK   8192            /* bytes, PSRAM (audit)         */
#define BLEH_TASK_PRIO    5
#define BLEH_HTTP_BUF     1024            /* esp_http_client rx/tx buffers*/

/* ---- state (PSRAM per §2; the TCB is a FreeRTOS object -> internal) ------------- */

static uint8_t s_rx_storage[BLEH_RX_STORAGE] EXT_RAM_BSS_ATTR;
static uint8_t s_frame_rx[BLEH_FRAME_BUF] EXT_RAM_BSS_ATTR;
static uint8_t s_frame_tx[BLEH_FRAME_BUF] EXT_RAM_BSS_ATTR;
static uint8_t s_slice[BLEH_SLICE] EXT_RAM_BSS_ATTR;
static StackType_t s_stack[BLEH_TASK_STACK] EXT_RAM_BSS_ATTR;
static StaticTask_t s_tcb; /* internal: FreeRTOS object */
static bleh_core_t s_core EXT_RAM_BSS_ATTR;
static char s_url[BLEH_PATH_MAX + 32] EXT_RAM_BSS_ATTR;
static char s_rsp_ct[BLEH_CT_MAX] EXT_RAM_BSS_ATTR;

static TaskHandle_t s_task;
static int s_ch = -1;
static bool s_registered;
static volatile bool s_run;
static volatile bool s_secured;
static esp_http_client_handle_t s_client;

/* ---- the HTTP side: esp_http_client over loopback ------------------------------- */

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL &&
        strcasecmp(evt->header_key, "Content-Type") == 0 &&
        evt->header_value != NULL)
    {
        strlcpy(s_rsp_ct, evt->header_value, sizeof(s_rsp_ct));
    }

    return ESP_OK;
}

static esp_http_client_method_t method_of(const char *m)
{
    if (strcmp(m, "POST") == 0)   return HTTP_METHOD_POST;
    if (strcmp(m, "PUT") == 0)    return HTTP_METHOD_PUT;
    if (strcmp(m, "DELETE") == 0) return HTTP_METHOD_DELETE;
    return HTTP_METHOD_GET;
}

static esp_err_t http_open(void *ctx, const bleh_req_t *req)
{
    (void)ctx;

    snprintf(s_url, sizeof(s_url), "http://127.0.0.1:%u%s",
             (unsigned)http_server_manager_port(), req->path);
    s_rsp_ct[0] = '\0';

    esp_http_client_config_t cfg =
    {
        .url = s_url,
        .method = method_of(req->method),
        .timeout_ms = BLEH_IDLE_TIMEOUT_MS,
        .event_handler = on_http_event,
        .buffer_size = BLEH_HTTP_BUF,
        .buffer_size_tx = BLEH_HTTP_BUF,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        .user_agent = "WiCAN-BLE/1",
    };

    s_client = esp_http_client_init(&cfg);

    if (s_client == NULL)
    {
        return ESP_FAIL;
    }

    if (req->ct[0] != '\0')
    {
        esp_http_client_set_header(s_client, "Content-Type", req->ct);
    }

    /* lets a handler know the request came over BLE (none checks today) */
    esp_http_client_set_header(s_client, "X-WiCAN-Transport", "ble");

    esp_err_t err = esp_http_client_open(s_client, (int)req->len);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "loopback open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }

    return err;
}

static esp_err_t http_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;

    while (len > 0)
    {
        int w = esp_http_client_write(s_client, (const char *)data, (int)len);

        if (w <= 0)
        {
            return ESP_FAIL;
        }

        data += w;
        len -= (size_t)w;
    }

    return ESP_OK;
}

static esp_err_t http_fetch(void *ctx, int *status, char *ct, size_t ct_cap,
                            int64_t *len)
{
    (void)ctx;

    int64_t total = esp_http_client_fetch_headers(s_client);

    if (total < 0)
    {
        ESP_LOGW(TAG, "loopback response head failed (%lld)", (long long)total);
        return ESP_FAIL;
    }

    *status = esp_http_client_get_status_code(s_client);
    *len = esp_http_client_is_chunked_response(s_client) ? -1 : total;
    strlcpy(ct, s_rsp_ct, ct_cap);
    return ESP_OK;
}

static int http_read(void *ctx, uint8_t *dst, size_t cap)
{
    (void)ctx;

    /* esp_http_client_read() returns 0 not only at the end of the body but
       also at a CHUNK boundary of a chunked response (/api/fs/download and
       /api/logs/ring stream 1 KB chunks): a body ends only when the client
       says the data is complete. Bench-caught 2026-09-21 (an SD download
       ended after 57 KB with LAST). */
    for (int i = 0; i < 300; i++)
    {
        int r = esp_http_client_read(s_client, (char *)dst, (int)cap);

        if (r < 0)
        {
            return -1; /* incl. -ESP_ERR_HTTP_EAGAIN: a stalled handler */
        }

        if (r > 0 || esp_http_client_is_complete_data_received(s_client))
        {
            return r;
        }

        vTaskDelay(pdMS_TO_TICKS(10)); /* between chunks */
    }

    return -1; /* 3 s without body bytes or an end: give up */
}

static void http_close(void *ctx)
{
    (void)ctx;

    if (s_client != NULL)
    {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static const bleh_ops_t OPS =
{
    .open = http_open, .write = http_write, .fetch = http_fetch,
    .read = http_read, .close = http_close,
};

/* ---- the BLE side ----------------------------------------------------------------- */

static bool emit(void *arg, const uint8_t *frame, size_t n)
{
    (void)arg;
    return ble_manager_channel_write(s_ch, frame, n) == (int)n;
}

static void on_channel_event(int id, ble_manager_channel_event_t evt, void *arg)
{
    (void)id;
    (void)arg;
    s_secured = (evt == BLE_MANAGER_CH_SECURED); /* host task: flag only */
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void tunnel_task(void *arg)
{
    (void)arg;

    ESP_LOGD(TAG, "tunnel task up");

    while (s_run)
    {
        /* while a response streams, poll the channel briefly between pump
           rounds so the app's CREDIT / ABORT frames are taken promptly */
        bool responding = (s_core.state == BLEH_ST_RESPONDING);
        int n = ble_manager_channel_read(s_ch, s_slice, sizeof(s_slice),
                                         responding ? 1 : 100);

        if (n < 0)
        {
            bleh_core_link_down(&s_core); /* idempotent while down */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!responding)
        {
            /* the mode the central selected with its CCCD and the link's
               PDU size shape the NEXT response (sampled at its REQ) */
            bleh_core_set_link(&s_core,
                               ble_manager_channel_out_mode(s_ch) ==
                                   BLE_MANAGER_CH_OUT_NOTIFY,
                               ble_manager_channel_pdu_max());
        }

        if (n > 0)
        {
            bleh_core_rx(&s_core, s_slice, (size_t)n, now_ms());
        }

        if (s_core.state == BLEH_ST_RESPONDING)
        {
            (void)bleh_core_pump(&s_core, now_ms());
        }

        if (n == 0)
        {
            bleh_core_tick(&s_core, now_ms());
        }
    }

    bleh_core_link_down(&s_core);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle ---------------------------------------------------------------------- */

esp_err_t ble_http_init(void)
{
    static const log_descriptor_t LOG_DESC = { "ble_http", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return bleh_settings_register();
}

esp_err_t ble_http_start(void)
{
    if (!bleh_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (!bleh_settings_config()->enabled)
    {
        ESP_LOGI(TAG, "disabled; no BLE channel");
        return ESP_OK;
    }

    if (!s_registered)
    {
        ble_manager_channel_desc_t desc =
        {
            .name = "http",
            .uuid_out = BLEH_UUID_OUT,
            .uuid_in = BLEH_UUID_IN,
            .rx_storage = s_rx_storage,
            .rx_size = sizeof(s_rx_storage),
            .on_event = on_channel_event,
            /* the app picks with its CCCD: indications (confirmed, slow)
               or notifications (fast; protocol v2 credits + counter) */
            .out_modes = BLE_MANAGER_CH_OUT_INDICATE | BLE_MANAGER_CH_OUT_NOTIFY,
        };
        esp_err_t err = ble_manager_channel_register(&desc, &s_ch);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "channel not registered: %s", esp_err_to_name(err));
            return err;
        }

        s_registered = true;
        bleh_core_init(&s_core, &OPS, NULL, emit, NULL, s_frame_rx, s_frame_tx);
    }

    if (s_task != NULL)
    {
        return ESP_OK;
    }

    s_run = true;
    s_task = xTaskCreateStatic(tunnel_task, "ble_http",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               BLEH_TASK_PRIO, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started (channel %d, FFF3/FFF4, loopback :%u)", s_ch,
             (unsigned)http_server_manager_port());
    return ESP_OK;
}

esp_err_t ble_http_stop(void)
{
    s_run = false;

    for (int i = 0; i < 30 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

esp_err_t ble_http_status(ble_http_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = bleh_settings_is_configured() &&
                   bleh_settings_config()->enabled;
    out->registered = s_registered;
    out->link_secured = s_secured;
    out->busy = (s_core.state != BLEH_ST_IDLE);
    strlcpy(out->method, s_core.req.method, sizeof(out->method));
    strlcpy(out->path, s_core.req.path, sizeof(out->path));
    out->body_expected = s_core.req.len;
    out->body_received = s_core.received;
    out->requests = s_core.ctr.requests;
    out->responses = s_core.ctr.responses;
    out->errors = s_core.ctr.errors;
    out->resync = s_core.ctr.resync;
    out->aborts = s_core.ctr.aborts;
    out->timeouts = s_core.ctr.timeouts;
    out->bytes_in = s_core.ctr.bytes_in;
    out->bytes_out = s_core.ctr.bytes_out;
    out->last_status = s_core.ctr.last_status;
    out->holes = s_core.ctr.holes;
    out->credits_rx = s_core.ctr.credits_rx;
    out->credit_stalls = s_core.ctr.credit_stalls;
    out->out_mode = s_registered
                        ? ble_manager_channel_out_name(ble_manager_channel_out_mode(s_ch))
                        : "none";
    out->out_notify = s_registered &&
                      ble_manager_channel_out_mode(s_ch) == BLE_MANAGER_CH_OUT_NOTIFY;
    return ESP_OK;
}
