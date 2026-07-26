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
 * @file ota_manager_private.h
 * @brief Internal API: the PURE session state machine (host-testable —
 *        flash operations injected) and its contract with the esp_ota glue.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ota_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Injected flash backend (esp_ota on target, a recorder in host tests). */
typedef struct
{
    esp_err_t (*begin)(void *io, size_t total_size);
    esp_err_t (*write)(void *io, const void *data, size_t len);
    esp_err_t (*end)(void *io);   /* validate + set boot partition */
    void      (*abort)(void *io);
} ota_session_ops_t;

typedef struct
{
    const ota_session_ops_t *ops;
    void *io;
    ota_manager_state_t state;
    uint32_t received;
    uint32_t total;
    char error[64];
} ota_session_t;

/** Reset to IDLE with the given backend. */
void ota_session_init(ota_session_t *s, const ota_session_ops_t *ops,
                      void *io);

/**
 * Transition rules (host-tested):
 *  - begin: allowed from IDLE / FAILED / READY (retry & re-upload);
 *    ESP_ERR_INVALID_STATE while RECEIVING. Backend failure -> FAILED.
 *  - write: only while RECEIVING; overrun of an announced total, zero-len,
 *    or backend failure -> FAILED (+ backend abort).
 *  - end: only while RECEIVING; zero bytes ("empty image"), short of an
 *    announced total ("short image"), or backend failure -> FAILED.
 *  - abort: any state -> IDLE (backend abort when a session was open).
 */
esp_err_t ota_session_begin(ota_session_t *s, size_t total_size);
esp_err_t ota_session_write(ota_session_t *s, const void *data, size_t len);
esp_err_t ota_session_end(ota_session_t *s);
esp_err_t ota_session_abort(ota_session_t *s);

#ifdef __cplusplus
}
#endif
