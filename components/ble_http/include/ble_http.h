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
 * @file ble_http.h
 * @brief The HTTP API over BLE: a ble_manager stream channel ("http",
 *        FFF3 notify / FFF4 write) carrying framed HTTP requests that the
 *        device replays against its own web server over loopback
 *        (127.0.0.1) and streams the responses back. Every `/api/...` route
 *        in components/HTTP_API.md is reachable from a paired phone with
 *        no per-route code here - storage (`/api/fs/...`), settings, status,
 *        UDS, OTA. Wire protocol: BLE_HTTP_PROTOCOL.md.
 *
 * Settings ("ble_http", reboot-to-apply): enabled (default true - the
 * channel exists only on the paired, MITM-authenticated BLE link, which
 * BLE itself gates with `ble_manager.enabled`), cli.
 *
 * Composition: ble_http_init() after ble_manager_init(); ble_http_start()
 * after the settings boot pass and BEFORE ble_manager_start() (the channel
 * must be registered before the GATT table is built).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the "ble_http" settings + log descriptors. No radio, no task. */
esp_err_t ble_http_init(void);

/** Register the "http" channel with ble_manager and start the tunnel task
 *  (when enabled). Must run before ble_manager_start(). */
esp_err_t ble_http_start(void);

/** Stop the tunnel task (sleep path). The channel stays registered. */
esp_err_t ble_http_stop(void);

/** `blehttp` console command (registered from the settings on_apply). */
esp_err_t ble_http_register_cli(void);

typedef struct
{
    bool     enabled;
    bool     registered;       /* channel present in the GATT table   */
    bool     link_secured;     /* a paired central is connected       */
    bool     busy;             /* a request is in flight              */
    char     method[8];        /* of the active / last request        */
    char     path[96];         /* truncated for display               */
    uint32_t body_expected;    /* active upload: bytes announced      */
    uint32_t body_received;
    uint32_t requests;         /* REQ frames accepted                 */
    uint32_t responses;        /* RSP heads sent                      */
    uint32_t errors;           /* tunnel-side error responses         */
    uint32_t resync;           /* bad frame headers skipped           */
    uint32_t aborts;           /* client ABORTs                       */
    uint32_t timeouts;         /* idle body timeouts                  */
    uint32_t bytes_in;         /* payload bytes from the phone        */
    uint32_t bytes_out;        /* payload bytes to the phone          */
    int      last_status;      /* HTTP status of the last response    */
    uint32_t holes;            /* REQ_BODY counter gaps (v2)          */
    uint32_t credits_rx;       /* app CREDIT frames (notify mode)     */
    uint32_t credit_stalls;    /* download pauses on the OUT window   */
    const char *out_mode;      /* "indicate" | "notify" | "none"      */
    bool     out_notify;       /* the live OUT mode is notifications  */
} ble_http_status_t;

esp_err_t ble_http_status(ble_http_status_t *out);

#ifdef __cplusplus
}
#endif
