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
 * @file bridge_endpoints.h
 * @brief The adapter layer between providers and bridge_manager: every
 *        data interface, wrapped as a named jack (bridge endpoint).
 *
 * Providers stay bridge-agnostic (obd_chip, can_manager, ble_manager,
 * cmdline_manager, socket_manager, websocket_manager) — this
 * component knows both sides and registers the jacks:
 *
 *   fixed (init):   obd  can  ble  cli  usb_obd
 *   dynamic (start): one jack PER CONFIGURED socket server and WS
 *                    channel, under its configured name (defaults:
 *                    obd0 slcan0 gvret0 udp0 / ws_obd ws_can ws_cli)
 *
 * Add-on packs may register further jacks straight with bridge_manager
 * during their ext init hook (same pre-start window).
 *
 * It also owns the two adapters that are machinery rather than
 * wrappers: the USB port-B UART (`usb_obd`) and the CAN frame<->chunk
 * pump (`can`).
 *
 * Ordering contract (main composes): init after bridge_manager_init;
 * start after the settings boot pass (it reads the configured
 * socket/WS names) and BEFORE bridge_manager_start (registration is
 * pre-start only).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Log descriptor + the fixed jacks (obd/can/ble/cli/usb_obd/elm0/elm1). */
esp_err_t bridge_endpoints_init(void);

/** Register a jack per configured socket server / WS channel (names from
 *  the applied settings), then bring up the USB port-B UART + RX task. */
esp_err_t bridge_endpoints_start(void);

/** Configure the `can` jack's RX filter (mask==0 = monitor all, the
 *  default). Pre-bridge-start only — applies at the next subscribe.
 *  Used by mqtt_can's 1:1 filter mapping (meatpi 2026-07-22). */
void bep_can_set_filter(uint32_t filter, uint32_t mask, bool ext);

esp_err_t bridge_endpoints_stop(void);

#ifdef __cplusplus
}
#endif
