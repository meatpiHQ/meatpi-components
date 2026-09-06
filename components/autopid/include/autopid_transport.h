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
 * @file autopid_transport.h
 * @brief The OBD transport behind autopid: a passthrough to the MIC3624
 *        (obd_chip — single master, claim arbiter).
 *
 * Every autopid path (runner, DTC, passive filter) talks to the chip
 * through this one vtable, so the protocol code stays free of direct
 * obd_chip calls. request() collects to the '>' prompt with echo+prompt
 * STRIPPED; monitor mode = claim → subscribe(queue of obd_chunk_t) →
 * send("ATMA\r") → stream → stop → unsubscribe → release.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    esp_err_t (*request)(const char *cmd, char *resp, size_t resp_len,
                         TickType_t timeout);
    esp_err_t (*claim_monitor)(TickType_t timeout);
    esp_err_t (*release)(void);
    esp_err_t (*send)(const uint8_t *data, size_t len);
    esp_err_t (*subscribe)(QueueHandle_t q, const char *name);
    esp_err_t (*unsubscribe)(QueueHandle_t q);
    esp_err_t (*monitor_stop)(void);
} ap_transport_t;

/** The OBD transport (never NULL). */
const ap_transport_t *ap_be(void);

#ifdef __cplusplus
}
#endif
