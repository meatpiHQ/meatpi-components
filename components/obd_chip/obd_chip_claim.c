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
 * @file obd_chip_claim.c
 * @brief The claim model (task §5): COMMAND / MONITOR / EXCLUSIVE arbitration
 *        of the chip's users, and the multi-command transaction hold. Split
 *        out of obd_chip.c 2026-10-28 (700-line rule); the registry and the
 *        fan-out stay there.
 *
 * Claims: obd_chip_request() claims COMMAND for one transaction; a bridge
 * running a monitor command claims MONITOR (a COMMAND then fails fast:
 * v1 policy "manual"); the firmware update claims EXCLUSIVE (fan-out
 * pauses, send() rejects). A transaction (obd_chip_txn_begin) is the task
 * that owns the COMMAND claim across several requests: a request from THAT
 * task neither claims nor releases. A transaction whose end() never ran
 * cannot brick the chip: the COMMAND claim is stolen after the max hold
 * (fail-open, like obd_gate's expiry).
 */
#include "obd_chip.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

typedef enum
{
    CLAIM_NONE = 0,
    CLAIM_COMMAND,
    CLAIM_MONITOR,
    CLAIM_EXCLUSIVE,
} claim_state_t;

#define OBD_TXN_MAX_US (12 * 1000 * 1000) /* > worst UDS transaction */

static portMUX_TYPE s_claim_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock */
static volatile claim_state_t s_claim = CLAIM_NONE;
static TaskHandle_t s_txn_task;
static int64_t      s_txn_start_us;

esp_err_t obd_core_claim(int type, uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    while (true)
    {
        bool granted = false;
        bool monitor_blocks = false;

        portENTER_CRITICAL(&s_claim_lock);

        /* a request from the transaction owner already holds the claim */
        if (type == CLAIM_COMMAND && s_txn_task != NULL &&
            s_txn_task == self)
        {
            granted = true;
        }
        /* a transaction whose end() never ran can't brick the chip: steal
           the COMMAND claim after the max hold (fail-open, like obd_gate) */
        else if (s_claim == CLAIM_COMMAND && s_txn_task != NULL &&
                 (esp_timer_get_time() - s_txn_start_us) > OBD_TXN_MAX_US)
        {
            s_txn_task = NULL;
            s_claim = (claim_state_t)type;
            granted = true;
        }
        else if (s_claim == CLAIM_NONE)
        {
            s_claim = (claim_state_t)type;
            granted = true;
        }
        else if (s_claim == CLAIM_MONITOR && type == CLAIM_COMMAND)
        {
            monitor_blocks = true;
        }

        portEXIT_CRITICAL(&s_claim_lock);

        if (granted)
        {
            return ESP_OK;
        }

        /* v1 policy "manual": a held MONITOR is never auto-interrupted,
           commands fail fast so callers can surface "bus busy" (task §5) */
        if (monitor_blocks)
        {
            return ESP_ERR_INVALID_STATE;
        }

        if (xTaskGetTickCount() >= deadline)
        {
            return (s_claim == CLAIM_EXCLUSIVE) ? ESP_ERR_INVALID_STATE
                                                : ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void obd_core_release(void)
{
    portENTER_CRITICAL(&s_claim_lock);

    /* a nested request inside a held transaction does NOT release: the
       transaction owns the claim until obd_chip_txn_end() */
    if (s_txn_task == NULL || s_txn_task != xTaskGetCurrentTaskHandle())
    {
        s_claim = CLAIM_NONE;
    }

    portEXIT_CRITICAL(&s_claim_lock);
}

bool obd_core_exclusive_held(void)
{
    return s_claim == CLAIM_EXCLUSIVE;
}

int obd_core_claim_state(void)
{
    return (int)s_claim;
}

esp_err_t obd_chip_claim(obd_claim_t type, TickType_t timeout)
{
    return obd_core_claim((int)type + 1 /* CLAIM_NONE offset */,
                          timeout * portTICK_PERIOD_MS);
}

esp_err_t obd_chip_release(void)
{
    obd_core_release();
    return ESP_OK;
}

esp_err_t obd_chip_txn_begin(TickType_t timeout)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool already;

    portENTER_CRITICAL(&s_claim_lock);
    already = (s_txn_task == self);
    portEXIT_CRITICAL(&s_claim_lock);

    if (already)
    {
        return ESP_ERR_INVALID_STATE; /* one transaction per task */
    }

    /* take the COMMAND claim as a normal requester (no txn owner yet), then
       become the owner: nested obd_chip_request() calls skip claim/release */
    esp_err_t err = obd_core_claim(CLAIM_COMMAND,
                                   timeout * portTICK_PERIOD_MS);

    if (err != ESP_OK)
    {
        return err;
    }

    portENTER_CRITICAL(&s_claim_lock);
    s_txn_task = self;
    s_txn_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_claim_lock);
    return ESP_OK;
}

void obd_chip_txn_end(void)
{
    portENTER_CRITICAL(&s_claim_lock);

    if (s_txn_task == xTaskGetCurrentTaskHandle())
    {
        s_txn_task = NULL;
        s_claim = CLAIM_NONE;
    }

    portEXIT_CRITICAL(&s_claim_lock);
}
