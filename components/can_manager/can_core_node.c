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
 * @file can_core_node.c
 * @brief The life of the TWAI node handle: the start on the configured ISR
 *        core, the stop, and the lock between those two and whoever reads
 *        the node's status from another task. Start and stop moved here from
 *        can_core_driver.c 2026-10-05 (700-line rule), with the lock.
 *
 * Why the lock. The link policy bounces the node (delete + create, on the RX
 * task) whenever it changes the bitrate candidate or the mode, and
 * can_core_get_stats() reads the node's error counters from any task: the
 * bus guard's probe every 10 ms while a listener finds a truck's bitrate,
 * GET /api/can, the CLI. twai_node_delete() lets the HAL go (its register
 * pointer becomes NULL) and frees the node before our handle is cleared: a
 * status read in that window loads through NULL + 0x3c, the TEC register.
 * Caught 2026-10-05 with the console on (`LoadProhibited`, twai_hal_get_tec
 * under can_manager_probe, 0.27 s after a detection's end on a 250 kbit/s
 * bus: the listener starts at 500 and is bounced to 250 while the probe
 * polls). The reset nobody could explain on 2026-10-03 sat at the same spot.
 *
 * Transmits have their own gate (can_core_link.c: tx_open / tx_users); the
 * RX task's own uses (recovery) are on the task that bounces.
 */
#include "can_core.h"
#include "can_core_driver.h"

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "can_core";

#define CAN_NODE_QUIESCE_WAIT_MS 50 /* the restart path may not block: a
                                       bounce in flight is given this long */

/* around every create / delete of can_drv_node and every use of it by a
   task that is not the one bouncing it. Created by the first start (init,
   serialized by its callers); a reader that finds none has no node to read. */
static SemaphoreHandle_t s_node_mtx;
static StaticSemaphore_t s_node_mtx_buf;   /* internal: FreeRTOS object */

bool can_drv_node_info(twai_node_status_t *info, twai_node_record_t *rec)
{
    bool ok = false;

    if (s_node_mtx == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_node_mtx, portMAX_DELAY);

    if (can_drv_node != NULL)
    {
        ok = (twai_node_get_info(can_drv_node, info, rec) == ESP_OK);
    }

    xSemaphoreGive(s_node_mtx);
    return ok;
}

/* ---- ISR core affinity (2026-07-21 experiment) ---------------------------
 * esp_intr_alloc binds the node ISR to the CALLING core. The default
 * (core 0) inherits WiFi/BT/USB/i2c neighbours and their slot pool;
 * CONFIG_WICAN_CAN_ISR_CORE=1 runs the driver init on a pinned
 * one-shot task so the CAN interrupt (and rx task) live on core 1. */
#if CONFIG_WICAN_CAN_ISR_CORE != 0
typedef struct
{
    can_core_handle_t *handle;
    uint32_t           baud_kbps;
    bool               listen_only;
    elm327_err_t       result;
    SemaphoreHandle_t  done;
} can_start_ctx_t;

static void can_driver_start_trampoline(void *arg)
{
    can_start_ctx_t *ctx = (can_start_ctx_t *)arg;

    ctx->result = can_drv_create(ctx->handle, ctx->baud_kbps,
                                 ctx->listen_only);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}
#endif

elm327_err_t can_drv_start_on_core(can_core_handle_t *handle,
                                      uint32_t baud_kbps, bool listen_only)
{
    elm327_err_t err;

    if (s_node_mtx == NULL)
    {
        s_node_mtx = xSemaphoreCreateMutexStatic(&s_node_mtx_buf);
    }

    /* held by the caller while the node comes up, on this task or on the
       one-shot task below */
    xSemaphoreTake(s_node_mtx, portMAX_DELAY);

#if CONFIG_WICAN_CAN_ISR_CORE == 0
    err = can_drv_create(handle, baud_kbps, listen_only);
#else
    static StaticSemaphore_t s_done_buf;
    can_start_ctx_t ctx =
    {
        .handle = handle,
        .baud_kbps = baud_kbps,
        .listen_only = listen_only,
        .result = ELM327_ERR_CAN,
        .done = xSemaphoreCreateBinaryStatic(&s_done_buf),
    };

    if (xTaskCreatePinnedToCore(can_driver_start_trampoline, "can_init",
                                3072, &ctx, 10, NULL,
                                CONFIG_WICAN_CAN_ISR_CORE) != pdPASS)
    {
        /* fallback: this core */
        err = can_drv_create(handle, baud_kbps, listen_only);
    }
    else
    {
        (void)xSemaphoreTake(ctx.done, portMAX_DELAY);
        err = ctx.result;
    }
#endif

    xSemaphoreGive(s_node_mtx);

    if (err == ELM327_OK)
    {
        handle->node_baud_kbps = baud_kbps;
    }

    return err;
}

void can_drv_stop(can_core_handle_t *handle)
{
    if (!handle || can_drv_node == NULL)
    {
        return;
    }

    /* (a node exists, so the lock does) no status read may be inside the
       driver while the node goes, and none may start until the handle is
       cleared */
    xSemaphoreTake(s_node_mtx, portMAX_DELAY);
    (void)twai_node_disable(can_drv_node);

    esp_err_t ret = twai_node_delete(can_drv_node);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "twai_node_delete failed: %d", ret);
    }

    can_drv_node = NULL;
    xSemaphoreGive(s_node_mtx);

    /* the driver leaves the TX pad floating: hold it recessive ourselves */
    can_drv_tx_recessive(handle->config.tx_gpio);
}

void can_drv_quiesce(void)
{
    /* the restart path: a bounce in flight gets a moment, then the restart
       goes on without us (the node that bounce creates dies with the
       restart) */
    if (s_node_mtx == NULL ||
        xSemaphoreTake(s_node_mtx,
                       pdMS_TO_TICKS(CAN_NODE_QUIESCE_WAIT_MS)) != pdTRUE)
    {
        return;
    }

    if (can_drv_node != NULL)
    {
        (void)twai_node_disable(can_drv_node);
    }

    xSemaphoreGive(s_node_mtx);
}

#endif /* ESP_PLATFORM */
