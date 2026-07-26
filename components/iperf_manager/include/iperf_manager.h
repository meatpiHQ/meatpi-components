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
 * @file iperf_manager.h
 * @brief WiCAN link-throughput measurement (feature component).
 *
 * Thin lifecycle + CLI wrapper around the managed `espressif/iperf`
 * engine (the esp-idf iperf example matured into a component;
 * **iperf2-compatible** — the PC/Pi side must run iperf 2.x, NOT
 * iperf3; default port 5001). Nothing runs until the operator starts a
 * session from the console:
 *
 *     iperf -s [-u] [-p port] [-t secs] [-i secs]        server
 *     iperf -c <host> [-u] [-p port] [-t secs] [-i secs]
 *           [-l len] [-b Mbps]                            client
 *     iperf -r                                            live report
 *     iperf -a                                            abort all
 *
 * Use cases: DUT<->PC over USB-NCM (192.168.82.x), DUT<->bench-Pi over
 * WiFi, and later the ESPNetlink LTE uplink. The server listens on
 * every netif — it is operator-started, time-bounded, and never
 * persists across reboots (no autostart setting on purpose: an open
 * traffic sink is a diagnostic, not a service).
 *
 * Interval reports print asynchronously via the engine's default
 * stdout writer (visible on the serial console); remote CLI sessions
 * (ws_cli/TCP/BLE) poll `iperf -r` for cumulative numbers instead.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register settings ("iperf_manager") + log descriptors. No network. */
esp_err_t iperf_manager_init(void);

/** Lifecycle uniformity (§3); passive until a CLI-started session.
 *  ESP_ERR_INVALID_STATE when unconfigured (§4.3 step 5). */
esp_err_t iperf_manager_start(void);

/** Abort any running session. */
esp_err_t iperf_manager_stop(void);

/** Register the `iperf` console command. Called INTERNALLY on the
 *  settings boot apply when the `cli` setting is true (default). */
esp_err_t iperf_manager_register_cli(void);

#ifdef __cplusplus
}
#endif
