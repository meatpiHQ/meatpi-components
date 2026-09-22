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
 * @file ble_j2534.c
 * @brief The BLE transport for j2534_server: a j2534_transport_t whose
 *        read/write are the ble_manager channel calls, and a session task
 *        that waits for a secured link, drains stale bytes, then blocks
 *        in j2534_server_serve_transport() for the session (the same
 *        shape as usb_cdc_device's CDC-ACM session task).
 */
#include "ble_j2534.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "j2534_proto.h"
#include "j2534_server.h"
#include "log_manager.h"

static const char *TAG = "ble_j2534";

#define BLJ_UUID_OUT     0xFFF5   /* notify: device -> tester            */
#define BLJ_UUID_IN      0xFFF6   /* write:  tester -> device            */
/* two maximal request frames (12 + 4 + 24 + 4128 each): write-without-
 * response has no app-level backpressure, so a tester may land a whole
 * request while the session task is still writing the previous ACK */
#define BLJ_RX_STORAGE   9216
#define BLJ_TASK_STACK   4096     /* bytes, PSRAM; no j2534_msg_t on it  */
#define BLJ_TASK_PRIO    6        /* == usb_cdc_device's session task    */

static uint8_t s_rx_storage[BLJ_RX_STORAGE] EXT_RAM_BSS_ATTR;
static StackType_t s_stack[BLJ_TASK_STACK] EXT_RAM_BSS_ATTR;
static StaticTask_t s_tcb; /* internal: FreeRTOS object */
static TaskHandle_t s_task;

static int s_ch = -1;
static bool s_registered;
static volatile bool s_run;
static volatile bool s_link_up;
static volatile bool s_session_active;
static uint32_t s_sessions;

/* ---- the transport vtable -------------------------------------------------- */

static int blj_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    (void)ctx;

    if (!s_link_up || !s_run)
    {
        return -1; /* link down / stopping: the session ends */
    }

    return ble_manager_channel_read(s_ch, buf, n, timeout_ms);
}

static int blj_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;

    if (!s_link_up)
    {
        return -1;
    }

    return ble_manager_channel_write(s_ch, buf, n);
}

static const j2534_transport_t s_transport =
{
    .name = "ble", .read = blj_read, .write = blj_write,
};

/* ---- link events (BT host task: flag + wake only) ---------------------------- */

static void on_channel_event(int id, ble_manager_channel_event_t evt, void *arg)
{
    (void)id;
    (void)arg;

    s_link_up = (evt == BLE_MANAGER_CH_SECURED);

    if (s_link_up && s_task != NULL)
    {
        xTaskNotifyGive(s_task);
    }
}

/* ---- session task ------------------------------------------------------------ */

static void session_task(void *arg)
{
    (void)arg;

    while (s_run)
    {
        if (!s_link_up)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }

        /* the channel discards a previous link's leftovers on the first
           read after it dropped; make that read happen here so the
           session's first frame is the tester's HELLO */
        uint8_t junk[16];

        while (ble_manager_channel_read(s_ch, junk, sizeof(junk), 0) > 0)
        {
        }

        ESP_LOGI(TAG, "BLE tester attached");
        s_session_active = true;
        j2534_server_serve_transport(&s_transport); /* blocks for the session */
        s_session_active = false;
        s_sessions++;
        ESP_LOGI(TAG, "BLE session ended");
        /* returns at once when the server is disabled or another
           transport holds the session: do not spin on a live link */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle ----------------------------------------------------------------- */

esp_err_t ble_j2534_init(void)
{
    static const log_descriptor_t LOG_DESC = { "ble_j2534", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return ESP_OK;
}

esp_err_t ble_j2534_start(void)
{
    if (s_task != NULL)
    {
        return ESP_OK;
    }

    /* the setting is known only after the settings boot pass, which runs
       between init and start; start is ordered before ble_manager_start()
       so the channel still lands in the GATT table (bench-caught: an
       init-time check always read false, 2026-09-21) */
    if (!s_registered)
    {
        if (!j2534_server_is_enabled())
        {
            ESP_LOGI(TAG, "not registered (j2534_server.enabled=false)");
            return ESP_OK;
        }

        ble_manager_channel_desc_t desc =
        {
            .name = "j2534",
            .uuid_out = BLJ_UUID_OUT,
            .uuid_in = BLJ_UUID_IN,
            .rx_storage = s_rx_storage,
            .rx_size = sizeof(s_rx_storage),
            .on_event = on_channel_event,
            /* INDICATE only: the J2534 wire protocol has no credit or
               frame-counter message, so it keeps the confirmed PDU; its
               traffic is small (see ble_http for the notify design) */
            .out_modes = BLE_MANAGER_CH_OUT_INDICATE,
        };
        esp_err_t err = ble_manager_channel_register(&desc, &s_ch);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "channel not registered: %s", esp_err_to_name(err));
            return err; /* log-and-degrade: J2534 stays on TCP / serial */
        }

        s_registered = true;
    }

    s_run = true;
    s_task = xTaskCreateStatic(session_task, "ble_j2534",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               BLJ_TASK_PRIO, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started (channel %d, FFF5/FFF6)", s_ch);
    return ESP_OK;
}

esp_err_t ble_j2534_stop(void)
{
    s_run = false;
    s_link_up = false; /* an active session's next read returns <0 */

    for (int i = 0; i < 30 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

esp_err_t ble_j2534_status(ble_j2534_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    out->registered = s_registered;
    out->link_up = s_link_up;
    out->session_active = s_session_active;
    out->sessions = s_sessions;
    return ESP_OK;
}
