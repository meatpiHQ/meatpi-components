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
 * @file ble_central_bench.c
 * @brief Lifecycle, state and the test task. Commands (connect /
 *        disconnect / run) are queued from any context and executed on
 *        the component's own PSRAM-stack task; the NimBLE host task only
 *        flags, counts and copies (the callbacks at the bottom). The
 *        timed tests live in ble_central_bench_modes.c, the radio side in
 *        ble_central_bench_gap.c.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "ble_central_bench_private.h"

static const char *TAG = BCB_TAG;

/* ---- state ---------------------------------------------------------------------- */

typedef enum { CMD_CONNECT, CMD_DISCONNECT, CMD_RUN } cmd_kind_t;

typedef struct
{
    cmd_kind_t kind;
    bcb_mode_t mode;
    uint32_t   seconds;
    uint32_t   size;
    uint8_t    out;         /* tunnel modes: BCB_OUT_*                     */
} cmd_t;

#define EV_CONNECTED    BIT0
#define EV_SECURED      BIT1
#define EV_DISCOVERED   BIT2
#define EV_DISCONNECTED BIT3
#define EV_PAIR_FAILED  BIT4

static bcb_status_t        s_st;
static SemaphoreHandle_t   s_lock;
static QueueHandle_t       s_cmds;
static EventGroupHandle_t  s_link;
static TaskHandle_t        s_task;
static StreamBufferHandle_t s_http_rx;          /* FFF3 bytes for the tunnel client */
static bool                s_started;

void bcb_core_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void bcb_core_unlock(void) { xSemaphoreGive(s_lock); }
bcb_status_t *bcb_core_status(void) { return &s_st; }

void bcb_core_set_state(bcb_state_t s)
{
    bcb_core_lock();
    s_st.state = s;
    bcb_core_unlock();
}

static char s_diag[BCB_DIAG_LINES][BCB_DIAG_LEN];
static int  s_diag_head, s_diag_count;

void bcb_diag(const char *fmt, ...)
{
    char line[BCB_DIAG_LEN - 16]; /* leaves room for the ms prefix */
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);

    bcb_core_lock();
    snprintf(s_diag[s_diag_head], BCB_DIAG_LEN, "%lu %s",
             (unsigned long)(esp_timer_get_time() / 1000), line);
    s_diag_head = (s_diag_head + 1) % BCB_DIAG_LINES;

    if (s_diag_count < BCB_DIAG_LINES)
    {
        s_diag_count++;
    }

    bcb_core_unlock();
}

int bcb_diag_lines(char (*out)[BCB_DIAG_LEN], int cap)
{
    int n = 0;

    bcb_core_lock();

    int start = (s_diag_head - s_diag_count + BCB_DIAG_LINES) % BCB_DIAG_LINES;

    for (int i = 0; i < s_diag_count && n < cap; i++)
    {
        memcpy(out[n++], s_diag[(start + i) % BCB_DIAG_LINES], BCB_DIAG_LEN);
    }

    bcb_core_unlock();
    return n;
}

void ble_central_bench_get_status(bcb_status_t *out)
{
    bcb_core_lock();
    *out = s_st;
    bcb_core_unlock();
}

bool ble_central_bench_is_enabled(void)
{
    return bcb_settings_is_configured() && bcb_settings_config()->enabled;
}

const char *ble_central_bench_state_name(bcb_state_t s)
{
    static const char *N[] = { "off", "idle", "scanning", "connecting", "connected", "running" };

    return (unsigned)s < sizeof(N) / sizeof(N[0]) ? N[s] : "?";
}

const char *ble_central_bench_mode_name(bcb_mode_t m)
{
    static const char *N[] = { "none", "notify", "write", "read", "tunnel_up", "tunnel_down" };

    return (unsigned)m < sizeof(N) / sizeof(N[0]) ? N[m] : "?";
}

bcb_mode_t ble_central_bench_mode_parse(const char *s)
{
    for (int m = BCB_MODE_NOTIFY; m <= BCB_MODE_TUNNEL_DOWN; m++)
    {
        if (s != NULL && strcmp(s, ble_central_bench_mode_name((bcb_mode_t)m)) == 0)
        {
            return (bcb_mode_t)m;
        }
    }

    return BCB_MODE_NONE;
}

/* ---- the task ------------------------------------------------------------------- */

static void do_connect(void)
{
    xEventGroupClearBits(s_link, 0xFF);
    bcb_core_set_state(BCB_STATE_SCANNING);

    if (bcb_gap_connect() != ESP_OK)
    {
        bcb_core_set_state(BCB_STATE_IDLE);
        return;
    }

    EventBits_t b = xEventGroupWaitBits(s_link, EV_DISCOVERED | EV_DISCONNECTED | EV_PAIR_FAILED,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(45000));

    if (b & EV_DISCOVERED)
    {
        bcb_core_set_state(BCB_STATE_CONNECTED);
        return;
    }

    bcb_diag("connect did not complete (%s)",
             (b & EV_PAIR_FAILED) ? "pairing failed" : (b & EV_DISCONNECTED) ? "disconnected" : "timeout");
    bcb_gap_disconnect();
    bcb_core_set_state(BCB_STATE_IDLE);
}

static void do_run(const cmd_t *c)
{
    bcb_result_t r = { .mode = c->mode, .running = true };

    bcb_core_lock();
    s_st.last = r;
    s_st.state = BCB_STATE_RUNNING;
    bcb_core_unlock();

    ESP_LOGI(TAG, "run %s: %lu s, %lu B", ble_central_bench_mode_name(c->mode),
             (unsigned long)c->seconds, (unsigned long)c->size);

    switch (c->mode)
    {
        case BCB_MODE_NOTIFY:      bcb_mode_notify(c->seconds, c->size, &r); break;
        case BCB_MODE_WRITE:       bcb_mode_write(c->seconds, &r); break;
        case BCB_MODE_READ:        bcb_mode_read(c->seconds, &r); break;
        case BCB_MODE_TUNNEL_UP:
        case BCB_MODE_TUNNEL_DOWN: bcb_mode_tunnel(c->mode, c->seconds, c->size, c->out, &r); break;
        default: break;
    }

    r.running = false;
    ESP_LOGI(TAG, "%s %s: %lu B in %lu ms = %lu kbps (%lu ops, %lu errors, %lu retries)%s%s",
             ble_central_bench_mode_name(c->mode), r.ok ? "ok" : "FAIL",
             (unsigned long)r.bytes, (unsigned long)r.ms, (unsigned long)r.kbps,
             (unsigned long)r.count, (unsigned long)r.errors, (unsigned long)r.retries,
             r.detail[0] ? ": " : "", r.detail);

    bcb_core_lock();
    s_st.last = r;
    s_st.state = bcb_gap_connected() ? BCB_STATE_CONNECTED : BCB_STATE_IDLE;
    bcb_core_unlock();
}

static void bench_task(void *arg)
{
    cmd_t c;

    (void)arg;

    for (;;)
    {
        if (xQueueReceive(s_cmds, &c, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (c.kind)
        {
            case CMD_CONNECT:    do_connect(); break;
            case CMD_DISCONNECT: bcb_gap_disconnect(); break;
            case CMD_RUN:        do_run(&c); break;
        }
    }
}

/* ---- public control ------------------------------------------------------------- */

static esp_err_t post(const cmd_t *c)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueSend(s_cmds, c, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ble_central_bench_connect(void)
{
    bcb_status_t st;

    ble_central_bench_get_status(&st);

    if (st.state != BCB_STATE_IDLE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    cmd_t c = { .kind = CMD_CONNECT };

    return post(&c);
}

esp_err_t ble_central_bench_disconnect(void)
{
    cmd_t c = { .kind = CMD_DISCONNECT };

    return post(&c);
}

esp_err_t ble_central_bench_run(bcb_mode_t mode, uint32_t seconds, uint32_t size,
                                uint8_t out)
{
    bcb_status_t st;

    ble_central_bench_get_status(&st);

    if (st.state != BCB_STATE_CONNECTED || mode == BCB_MODE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (seconds == 0 || seconds > 600 || size == 0 || size > (8u << 20))
    {
        return ESP_ERR_INVALID_ARG;
    }

    cmd_t c = { .kind = CMD_RUN, .mode = mode, .seconds = seconds, .size = size,
                .out = (out == BCB_OUT_INDICATE) ? BCB_OUT_INDICATE : BCB_OUT_NOTIFY };

    return post(&c);
}

/* ---- callbacks from the host task (flag, count, copy) ------------------------------ */

void bcb_core_on_connected(const char *name, const char *addr, int8_t rssi)
{
    bcb_core_lock();
    strlcpy(s_st.peer.name, name, sizeof(s_st.peer.name));
    strlcpy(s_st.peer.addr, addr, sizeof(s_st.peer.addr));
    s_st.peer.rssi = rssi;
    s_st.peer.secured = false;
    s_st.counters.connects++;
    s_st.state = BCB_STATE_CONNECTING;
    bcb_core_unlock();
    xEventGroupSetBits(s_link, EV_CONNECTED);
}

void bcb_core_on_secured(bool ok, bool bonded)
{
    bcb_core_lock();
    s_st.peer.secured = ok;
    s_st.peer.bonded = bonded;

    if (ok) s_st.counters.pair_ok++; else s_st.counters.pair_fail++;

    bcb_core_unlock();
    xEventGroupSetBits(s_link, ok ? EV_SECURED : EV_PAIR_FAILED);
}

void bcb_core_on_discovered(const bcb_chars_t *chars, bool ok)
{
    bcb_core_lock();
    s_st.chars = *chars;
    bcb_core_unlock();
    xEventGroupSetBits(s_link, ok ? EV_DISCOVERED : EV_PAIR_FAILED);
}

void bcb_core_on_disconnected(int reason)
{
    bcb_core_lock();
    s_st.counters.disconnects++;
    s_st.counters.last_disconnect_reason = reason;
    s_st.peer.secured = false;
    memset(&s_st.chars, 0, sizeof(s_st.chars));

    if (s_st.state != BCB_STATE_RUNNING)
    {
        s_st.state = BCB_STATE_IDLE;
    }

    bcb_core_unlock();
    xEventGroupSetBits(s_link, EV_DISCONNECTED);
}

void bcb_core_on_link_params(uint16_t mtu, uint16_t itvl_units, uint8_t phy_tx,
                             uint8_t phy_rx, uint16_t dle_tx, uint16_t dle_rx)
{
    bcb_core_lock();

    if (mtu)        s_st.peer.mtu = mtu;
    if (itvl_units) s_st.peer.itvl_units = itvl_units;
    if (phy_tx)     s_st.peer.phy_tx = phy_tx;
    if (phy_rx)     s_st.peer.phy_rx = phy_rx;
    if (dle_tx)     s_st.peer.dle_tx = dle_tx;
    if (dle_rx)     s_st.peer.dle_rx = dle_rx;

    bcb_core_unlock();
}

void bcb_core_on_notify(uint16_t attr_handle, const uint8_t *data, size_t n)
{
    uint16_t fff1, fff3;

    bcb_core_lock();
    fff1 = s_st.chars.fff1;
    fff3 = s_st.chars.fff3;
    s_st.counters.notify_rx++;
    s_st.counters.notify_bytes += n;
    bcb_core_unlock();

    if (attr_handle == fff3)
    {
        /* the tunnel client's bytes; a full buffer is a bench failure, not a stall */
        (void)xStreamBufferSend(s_http_rx, data, n, 0);
    }
    else if (attr_handle == fff1)
    {
        bcb_modes_on_fff1(n);
    }
}

StreamBufferHandle_t bcb_core_http_rx(void)
{
    return s_http_rx;
}

/* ---- lifecycle -------------------------------------------------------------------- */

esp_err_t ble_central_bench_init(void)
{
    static const log_descriptor_t LOG_DESC = { BCB_TAG, ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    s_link = xEventGroupCreate();

    if (s_lock == NULL || s_cmds == NULL || s_link == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    return bcb_settings_register();
}

esp_err_t ble_central_bench_start(void)
{
    if (!ble_central_bench_is_enabled())
    {
        ESP_LOGI(TAG, "disabled");
        return ESP_OK;
    }

    uint8_t *storage = heap_caps_malloc(BCB_RX_STORAGE + 1, MALLOC_CAP_SPIRAM);
    StaticStreamBuffer_t *sb = heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (storage == NULL || sb == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_http_rx = xStreamBufferCreateStatic(BCB_RX_STORAGE + 1, 1, storage, sb);

    esp_err_t err = bcb_gap_start();

    if (err != ESP_OK)
    {
        return err;
    }

    if (xTaskCreateWithCaps(bench_task, "ble_bench", BCB_TASK_STACK, NULL, 5, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "started: target '%s', mtu %u, itvl %.2f ms, dle %d, phy %s",
             bcb_settings_config()->target, bcb_settings_config()->mtu,
             bcb_settings_config()->conn_itvl_units * 1.25,
             bcb_settings_config()->dle,
             bcb_settings_config()->phy_mask == BCB_PHY_2M ? "2m" : "1m");
    return ESP_OK;
}

esp_err_t ble_central_bench_stop(void)
{
    if (!s_started)
    {
        return ESP_OK;
    }

    bcb_gap_disconnect();
    bcb_gap_stop();
    s_started = false;
    bcb_core_set_state(BCB_STATE_OFF);
    return ESP_OK;
}
