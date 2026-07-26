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
 * @file autopid_backend.h
 * @brief The OBD transport behind autopid (settings `backend`).
 *
 * Implementations of one contract (the obd_chip surface autopid
 * already consumed):
 *   - "obd_chip": passthrough to the MIC3624 (single master, claim
 *     arbiter — the historical path). Always built in.
 *   - "elm327": an alternate AT-engine transport on can_manager's
 *     shared native-CAN bus, provided by an optional add-on component
 *     pack via ap_backend_provide() (see ext_manager.h). When no pack
 *     is present, selecting it falls back to obd_chip with a log line.
 *
 * The contract mirrors obd_chip semantics exactly: request() collects to
 * the '>' prompt with echo+prompt STRIPPED; monitor mode = claim →
 * subscribe(queue of obd_chunk_t) → send("ATMA\r") → stream → stop →
 * unsubscribe → release.
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
} ap_backend_t;

/** The active backend (never NULL; defaults to obd_chip). */
const ap_backend_t *ap_be(void);

/** Register the alternate backend (an add-on pack calls this once, at
 *  ext init, before autopid_start). @p start_fn brings the engine up
 *  lazily when autopid selects the backend. */
esp_err_t ap_backend_provide(const ap_backend_t *be,
                             esp_err_t (*start_fn)(void));

/** True when an alternate backend has been provided by a pack. */
bool ap_backend_alt_available(void);

/** Bring the provided alternate backend up (needs can_manager running).
 *  ESP_ERR_NOT_SUPPORTED when no pack provided one. On failure the
 *  caller should fall back to obd_chip + log. */
esp_err_t ap_backend_alt_start(void);

/** Select the transport: true = the provided alternate backend (silently
 *  stays on obd_chip when none is provided), false = obd_chip. */
void ap_backend_select(bool use_alt);

#ifdef __cplusplus
}
#endif
