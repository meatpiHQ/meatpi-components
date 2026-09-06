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
 * @file obd_gate.c
 * @brief Lifecycle + the RTOS wrapper around the pure gate core: a
 *        spinlock-guarded state machine polled at 10 ms while blocked.
 *        Contention is rare (one conversation is 50–300 ms) so a poll
 *        loop beats a semaphore's bookkeeping here.
 */
#include "obd_gate.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "obd_gate_private.h"

static const char *TAG = "obd_gate";

#define OG_POLL_MS 10

static og_core_t s_gate;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

esp_err_t obd_gate_init(void)
{
    static const log_descriptor_t LOG_DESC = { "obd_gate", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return og_settings_register();
}

bool obd_gate_enabled(void)
{
    return og_settings_enabled();
}

esp_err_t obd_gate_acquire(const void *owner, uint32_t wait_ms)
{
    if (!og_settings_enabled() || owner == NULL)
    {
        return ESP_OK;
    }

    int64_t start = now_ms();
    bool waited = false;

    for (;;)
    {
        bool got;

        portENTER_CRITICAL(&s_mux);
        got = og_core_try(&s_gate, owner, now_ms(), OBD_GATE_HOLD_MS);

        if (!got && !waited)
        {
            waited = true;
            s_gate.waits++;
        }

        portEXIT_CRITICAL(&s_mux);

        if (got)
        {
            return ESP_OK;
        }

        if (now_ms() - start >= (int64_t)wait_ms)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(OG_POLL_MS));
    }

    /* fail-open: take it anyway — a wedged holder must never brick the
       other requester; the clash risk returns for this one conversation */
    portENTER_CRITICAL(&s_mux);
    og_core_force(&s_gate, owner, now_ms(), OBD_GATE_HOLD_MS);
    uint32_t steals = s_gate.steals;
    portEXIT_CRITICAL(&s_mux);

    if ((steals % 16) == 1)
    {
        ESP_LOGW(TAG, "gate held past %u ms — proceeding anyway "
                 "(%lu steals)", (unsigned)wait_ms, (unsigned long)steals);
    }

    return ESP_OK;
}

void obd_gate_release(const void *owner)
{
    if (!og_settings_enabled() || owner == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    og_core_release(&s_gate, owner);
    portEXIT_CRITICAL(&s_mux);
}

void obd_gate_get_stats(obd_gate_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    out->acquires = s_gate.acquires;
    out->waits = s_gate.waits;
    out->steals = s_gate.steals;
    out->expiries = s_gate.expiries;
    portEXIT_CRITICAL(&s_mux);
}

/* ---- ESP-side engine hooks (ctx = the engine instance pointer) -------------- */

void obd_gate_engine_acquire(void *ctx)
{
    (void)obd_gate_acquire(ctx, OBD_GATE_WAIT_MS);
}

void obd_gate_engine_release(void *ctx)
{
    obd_gate_release(ctx);
}
