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
 * @file ble_manager_gatt_tx.c
 * @brief The device -> central TX primitives of the NimBLE backend:
 *        notify (bounded ENOMEM retry, VHCI ingress gate) and indicate (one
 *        in flight, confirmed), the self-expiring congestion window the IO
 *        layer paces on, and the link hooks the GAP handler calls. Split out
 *        of ble_manager_gatt_nimble.c (700-line rule, 2026-09-22).
 *
 * The one loss ever measured on this path was NOT here: the ESP32-S3
 * controller allocated its ACL TX buffers from the internal heap at every
 * transmit (CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=0) and dropped a PDU
 * silently when that failed on a device with ~8 KB free (btmon on the
 * central: 1-4 consecutive PDUs the host had counted never reached the air,
 * 7 of 24 downloads). Static buffers (12) fixed it: 0 of 12 afterwards.
 */
#include "ble_manager.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

#include "ble_manager_private.h"

/* how long one ENOMEM keeps the IO layer's pacing loop backing off */
#define BLM_CONGEST_WINDOW_MS 5
/* the legacy 50 x 2 ms budget of the data/CLI notifies */
#define BLM_NOTIFY_WAIT_MS    100

static volatile TickType_t s_congested_until; /* window end, 0 = clear */

/* one indication in flight per link: the sender takes s_ind_slot, the
   GAP NOTIFY_TX event (EDONE = confirmed, ETIMEOUT = the 30 s ATT
   timeout) gives s_ind_done; disconnect gives it too so a waiter wakes */
static SemaphoreHandle_t s_ind_slot;   /* internal: FreeRTOS objects */
static StaticSemaphore_t s_ind_slot_buf;
static SemaphoreHandle_t s_ind_done;
static StaticSemaphore_t s_ind_done_buf;
static volatile int s_ind_status;      /* the last confirmation status */

static void congestion_clear(void)
{
    s_congested_until = 0;
}

/* ---- hooks for the GAP handler ------------------------------------------------------ */

void blm_tx_init(void)
{
    if (s_ind_slot == NULL)
    {
        s_ind_slot = xSemaphoreCreateBinaryStatic(&s_ind_slot_buf);
        s_ind_done = xSemaphoreCreateBinaryStatic(&s_ind_done_buf);
        xSemaphoreGive(s_ind_slot);
    }
}

void blm_tx_on_link_reset(void)
{
    congestion_clear();

    if (s_ind_done != NULL)
    {
        s_ind_status = BLE_HS_ENOTCONN;
        xSemaphoreGive(s_ind_done); /* wake an indication waiter */
    }
}

void blm_tx_on_indicate_done(int status)
{
    s_ind_status = status;
    xSemaphoreGive(s_ind_done);
}

/* ---- the IO layer's view ------------------------------------------------------------- */

bool blm_gatt_congested(void)
{
    TickType_t until = s_congested_until;

    return blm_gatt_connected() && until != 0 &&
           (int32_t)(until - xTaskGetTickCount()) > 0;
}

int blm_gatt_free_packets(void)
{
    /* NimBLE exposes no controller-credit count: report a fixed credit
       while healthy; ENOMEM inside notify_* opens a short congestion
       window, which the IO layer's pacing loop honors and which expires
       by itself (never a latch). */
    return (blm_gatt_connected() && !blm_gatt_congested()) ? 8 : 0;
}

/* ---- notify / indicate ------------------------------------------------------------------ */

esp_err_t blm_gatt_notify_handle(uint16_t val_handle, const uint8_t *buf,
                                 uint16_t len, uint32_t max_wait_ms)
{
    if (!blm_gatt_connected() || !blm_gatt_secured() || val_handle == 0)
    {
        return ESP_ERR_INVALID_STATE; /* legacy CCCD-security equivalent */
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(max_wait_ms);

    for (;;)
    {
        if (!blm_gatt_connected())
        {
            return ESP_ERR_INVALID_STATE; /* dropped mid-wait */
        }

        /* The controller's VHCI ingress must have room BEFORE the host
           hands it the fragments: NimBLE's ESP glue only logs (debug) when
           esp_vhci_host_check_send_available() is false and sends anyway,
           and a packet the controller cannot take is gone without a
           trace (bench 2026-09-22, btmon on the central: 2-4 PDUs of a
           64 KB notify stream never reached the air while the host had
           counted them). Bluedroid's HCI layer waits on this check; we do
           the same for our notifications. */
        if (!esp_vhci_host_check_send_available())
        {
            s_congested_until = xTaskGetTickCount() +
                                pdMS_TO_TICKS(BLM_CONGEST_WINDOW_MS) + 1;

            if ((int32_t)(deadline - xTaskGetTickCount()) <= 0)
            {
                return ESP_ERR_TIMEOUT;
            }

            vTaskDelay(1);
            continue;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        int rc = (om == NULL) ? BLE_HS_ENOMEM
                              : ble_gatts_notify_custom(blm_gatt_conn_handle(),
                                                        val_handle, om);

        if (rc == 0)
        {
            congestion_clear();
            return ESP_OK;
        }

        if (rc != BLE_HS_ENOMEM)
        {
            return ESP_FAIL;
        }

        /* mbufs / LL credits exhausted: back off, self-expiring window */
        s_congested_until = xTaskGetTickCount() +
                            pdMS_TO_TICKS(BLM_CONGEST_WINDOW_MS) + 1;

        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0)
        {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t blm_gatt_indicate_handle(uint16_t val_handle, const uint8_t *buf,
                                   uint16_t len, uint32_t max_wait_ms)
{
    if (!blm_gatt_connected() || !blm_gatt_secured() || val_handle == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(max_wait_ms);

    /* one indication in flight on the link */
    if (xSemaphoreTake(s_ind_slot, pdMS_TO_TICKS(max_wait_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_ERR_TIMEOUT;

    while (blm_gatt_connected())
    {
        xSemaphoreTake(s_ind_done, 0); /* clear a stale confirmation */
        s_ind_status = -1;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        int rc = (om == NULL) ? BLE_HS_ENOMEM
                              : ble_gatts_indicate_custom(blm_gatt_conn_handle(),
                                                          val_handle, om);

        if (rc == 0)
        {
            /* wait for the central's confirmation (or the link to drop) */
            int32_t left = (int32_t)(deadline - xTaskGetTickCount());

            if (left < (int32_t)pdMS_TO_TICKS(100))
            {
                left = pdMS_TO_TICKS(100);
            }

            if (xSemaphoreTake(s_ind_done, (TickType_t)left) == pdTRUE &&
                s_ind_status == BLE_HS_EDONE)
            {
                congestion_clear();
                err = ESP_OK;
            }
            else
            {
                err = blm_gatt_connected() ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
            }

            break;
        }

        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY)
        {
            err = ESP_FAIL;
            break;
        }

        /* mbufs / credits / a stack-internal indication still pending:
           back off inside the same deadline */
        s_congested_until = xTaskGetTickCount() +
                            pdMS_TO_TICKS(BLM_CONGEST_WINDOW_MS) + 1;

        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (!blm_gatt_connected())
    {
        err = ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(s_ind_slot);
    return err;
}

esp_err_t blm_gatt_notify_data(const uint8_t *buf, uint16_t len)
{
    return blm_gatt_notify_handle(blm_svc_data_out_handle(), buf, len,
                                  BLM_NOTIFY_WAIT_MS);
}

esp_err_t blm_gatt_notify_cli(const uint8_t *buf, uint16_t len)
{
    return blm_gatt_notify_handle(blm_svc_cli_out_handle(), buf, len,
                                  BLM_NOTIFY_WAIT_MS);
}

