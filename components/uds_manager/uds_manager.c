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
 * @file uds_manager.c
 * @brief UDS protocol over a selectable transport: backend select, the
 *        0x78 responsePending loop, NRC decode, tester-present, and the
 *        one-transaction-at-a-time claim.
 */
#include "uds_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "can_isotp.h"
#include "can_manager.h"
#include "log_manager.h"

#include "uds_manager_private.h"
#include "uds_proto.h"
#include "uds_transport.h"

static const char *TAG = "uds_manager";

/* ---- state ---------------------------------------------------------------- */

static bool s_started;

/* one transaction / session at a time */
static SemaphoreHandle_t s_claim;
static StaticSemaphore_t s_claim_buf;

/* active tester-present session */
static esp_timer_handle_t s_tp_timer;
static uds_addr_t s_tp_addr;
static bool s_session;

/* ---- backend resolution ---------------------------------------------------- */

const char *uds_manager_backend_name(uds_backend_t b)
{
    switch (b)
    {
    case UDS_BACKEND_OBD_CHIP: return "obd_chip";
    case UDS_BACKEND_ISOTP:    return "isotp";
    default:                   return "auto";
    }
}

static const uds_transport_t *resolve_transport(void)
{
    bool can_up = (can_manager_core_handle() != NULL);
    /* fw ISO-TP needs the bus AND a registered provider (can_isotp.h) */
    bool isotp_ok = can_up && (can_isotp() != NULL);

    switch (uds_settings_config()->backend)
    {
    case UDS_BACKEND_OBD_CHIP:
        return uds_transport_obd();

    case UDS_BACKEND_ISOTP:
        if (isotp_ok) return uds_transport_isotp();
        ESP_LOGW(TAG, "isotp backend forced but unavailable (CAN down "
                      "or no ISO-TP provider) -> obd_chip");
        return uds_transport_obd();

    case UDS_BACKEND_AUTO:
    default:
        /* isotp when available (most capable), else the MIC */
        return isotp_ok ? uds_transport_isotp() : uds_transport_obd();
    }
}

uds_backend_t uds_manager_active_backend(void)
{
    const uds_transport_t *t = resolve_transport();

    if (t == uds_transport_isotp())                          return UDS_BACKEND_ISOTP;
    return UDS_BACKEND_OBD_CHIP;
}

/* ---- one transaction (assumes the claim is held) --------------------------- */

static esp_err_t do_transaction(const uds_transport_t *t,
                                const uds_addr_t *addr,
                                const uint8_t *req, size_t req_len,
                                uint8_t *resp, size_t resp_cap,
                                size_t *resp_len, const uds_opts_t *opts,
                                uds_result_t *result)
{
    const uds_config_t *cfg = uds_settings_config();
    uint32_t p2 = (opts && opts->p2_ms) ? opts->p2_ms : cfg->p2_ms;
    uint32_t p2star = (opts && opts->p2star_ms) ? opts->p2star_ms
                                                : cfg->p2star_ms;
    uint8_t max_pending = (opts && opts->max_pending) ? opts->max_pending
                                                      : 20;

    (void)max_pending; /* the transport caps its own pending loop */

    int64_t t0 = esp_timer_get_time();
    uint8_t pending = 0;
    esp_err_t err;

    /* the transport delivers the FINAL response (consuming 0x78
     * responsePending frames itself, with p2star per frame) */
    err = t->transceive(addr, req, req_len, resp, resp_cap, resp_len,
                        p2, p2star, &pending);

    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
        result->backend = t->name;
        result->pending_count = pending;
        result->elapsed_ms =
            (uint32_t)((esp_timer_get_time() - t0) / 1000);

        if (err == ESP_OK && *resp_len >= 1)
        {
            result->sid = resp[0];
            result->negative = uds_is_negative(resp, *resp_len);

            if (result->negative)
            {
                result->nrc = uds_nrc_of(resp, *resp_len);
                result->nrc_name = uds_nrc_name(result->nrc);
                result->sid = (*resp_len >= 2) ? resp[1] : resp[0];
            }
        }
    }

    return err;
}

/* ---- public request -------------------------------------------------------- */

esp_err_t uds_request(const uds_addr_t *addr,
                      const uint8_t *req, size_t req_len,
                      uint8_t *resp, size_t resp_cap, size_t *resp_len,
                      const uds_opts_t *opts, uds_result_t *result)
{
    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (addr == NULL || req == NULL || req_len == 0 || resp == NULL ||
        resp_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* a held session owns the claim already (uds_session_begin) */
    bool own_claim = !s_session;

    if (own_claim &&
        xSemaphoreTake(s_claim, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    const uds_transport_t *t = resolve_transport();
    esp_err_t err = t->open();

    if (err == ESP_OK)
    {
        err = do_transaction(t, addr, req, req_len, resp, resp_cap,
                             resp_len, opts, result);
    }

    if (own_claim)
    {
        xSemaphoreGive(s_claim);
    }

    return err;
}

/* ---- tester-present session ------------------------------------------------ */

static void tp_timer_cb(void *arg)
{
    (void)arg;

    if (!s_session)
    {
        return;
    }

    /* 3E 80 = TesterPresent, suppressPositiveResponse — fire and forget */
    const uint8_t tp[] = { 0x3E, 0x80 };
    uint8_t r[8];
    size_t rn = 0;
    const uds_transport_t *t = resolve_transport();

    (void)t->transceive(&s_tp_addr, tp, sizeof(tp), r, sizeof(r), &rn,
                        100, 100, NULL);
}

esp_err_t uds_session_begin(const uds_addr_t *addr)
{
    if (!s_started || addr == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_claim, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE; /* busy */
    }

    const uds_transport_t *t = resolve_transport();

    if (t->open() != ESP_OK)
    {
        xSemaphoreGive(s_claim);
        return ESP_ERR_INVALID_STATE;
    }

    s_tp_addr = *addr;
    s_session = true;

    if (s_tp_timer != NULL)
    {
        esp_timer_start_periodic(
            s_tp_timer,
            (uint64_t)uds_settings_config()->tester_present_ms * 1000);
    }

    return ESP_OK;
}

esp_err_t uds_session_end(void)
{
    if (!s_session)
    {
        return ESP_OK;
    }

    if (s_tp_timer != NULL)
    {
        esp_timer_stop(s_tp_timer);
    }

    s_session = false;
    xSemaphoreGive(s_claim);
    return ESP_OK;
}

/* ---- lifecycle ------------------------------------------------------------- */

esp_err_t uds_manager_init(void)
{
    static const log_descriptor_t LOG_DESC = { "uds_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    uds_events_register();  /* uds.request action + uds.response source */

    if (s_claim == NULL)
    {
        s_claim = xSemaphoreCreateMutexStatic(&s_claim_buf);
    }

    return uds_settings_register();
}

esp_err_t uds_manager_start(void)
{
    const uds_config_t *cfg = uds_settings_config();

    if (!uds_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    const esp_timer_create_args_t targs =
    {
        .callback = tp_timer_cb,
        .name     = "uds_tp",
    };

    (void)esp_timer_create(&targs, &s_tp_timer);

    s_started = true;
    ESP_LOGI(TAG, "started (backend=%s, p2=%lu p2*=%lu)",
             uds_manager_backend_name(cfg->backend),
             (unsigned long)cfg->p2_ms, (unsigned long)cfg->p2star_ms);
    return ESP_OK;
}

esp_err_t uds_manager_stop(void)
{
    (void)uds_session_end();
    s_started = false;
    return ESP_OK;
}
