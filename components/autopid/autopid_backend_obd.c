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
 * @file autopid_backend_obd.c
 * @brief backend "obd_chip" — passthrough to the MIC3624 (+ the backend
 *        registry/selector; the alternate "elm327" backend arrives via
 *        ap_backend_provide from an add-on pack).
 */
#include "autopid_backend.h"

#include "obd_chip.h"

static esp_err_t obd_claim_monitor(TickType_t timeout)
{
    return obd_chip_claim(OBD_CHIP_CLAIM_MONITOR, timeout);
}

static const ap_backend_t BE_OBD =
{
    .request       = obd_chip_request,
    .claim_monitor = obd_claim_monitor,
    .release       = obd_chip_release,
    .send          = obd_chip_send,
    .subscribe     = obd_chip_subscribe,
    .unsubscribe   = obd_chip_unsubscribe,
    .monitor_stop  = obd_chip_monitor_stop,
};

/* Alternate backend slot — filled by an add-on pack (ext init phase,
 * strictly before autopid_start reads it; no locking needed). */
static const ap_backend_t *s_alt;
static esp_err_t (*s_alt_start)(void);

static const ap_backend_t *s_active = &BE_OBD;

const ap_backend_t *ap_be(void)
{
    return s_active;
}

esp_err_t ap_backend_provide(const ap_backend_t *be,
                             esp_err_t (*start_fn)(void))
{
    if (be == NULL || start_fn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_alt = be;
    s_alt_start = start_fn;
    return ESP_OK;
}

bool ap_backend_alt_available(void)
{
    return s_alt != NULL;
}

esp_err_t ap_backend_alt_start(void)
{
    return (s_alt_start != NULL) ? s_alt_start() : ESP_ERR_NOT_SUPPORTED;
}

void ap_backend_select(bool use_alt)
{
    s_active = (use_alt && s_alt != NULL) ? s_alt : &BE_OBD;
}
