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
 * @file ble_j2534.h
 * @brief J2534 PassThru over BLE: glue between a ble_manager stream channel
 *        ("j2534", FFF5 notify / FFF6 write) and j2534_server's transport
 *        seam. Registers the channel (pre-start) when `j2534_server.enabled`
 *        is true and runs ONE tester session per secured BLE link from its
 *        own PSRAM-stacked task via j2534_server_serve_transport(). Same
 *        wire protocol as TCP / USB CDC-ACM (J2534_WIRE_PROTOCOL.md); the
 *        single-tester rule applies across all transports.
 *
 * No settings of its own: available iff `j2534_server.enabled`
 * (`allow_lan` gates only the TCP listener - a paired BLE link is a local
 * link like the AP / USB). Status via /api/j2534 `transport:"ble"`.
 *
 * Composition: ble_j2534_init() after j2534_server_init() and BEFORE
 * ble_manager_start(); ble_j2534_start() after j2534_server_start().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The log descriptor only (the setting is not known before the settings
 *  boot pass). */
esp_err_t ble_j2534_init(void);

/** Register the "j2534" channel with ble_manager (iff j2534_server is
 *  enabled) and start the session task. Must run AFTER the settings boot
 *  pass and BEFORE ble_manager_start(); a session is served once
 *  j2534_server_start() has run. */
esp_err_t ble_j2534_start(void);

/** Sleep path: ends an active BLE session, stops the task. */
esp_err_t ble_j2534_stop(void);

typedef struct
{
    bool     registered;     /* channel present in the GATT table       */
    bool     link_up;        /* a secured central is connected          */
    bool     session_active; /* j2534_server is serving this link       */
    uint32_t sessions;       /* sessions served since boot              */
} ble_j2534_status_t;

esp_err_t ble_j2534_status(ble_j2534_status_t *out);

#ifdef __cplusplus
}
#endif
