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
 * @file api_http.h
 * @brief WiCAN device HTTP API glue (Architecture §10, HTTP_API.md §9).
 *
 * Implements the core components' `/api` routes on top of
 * http_server_manager: settings (get/put/schema/list/submit), device status,
 * restart history + reboot, log-manager runtime knobs + crash-ring dump, and
 * the read-only filesystem browse endpoints. Core components never touch
 * HTTP — this glue depends DOWN on them (never the reverse).
 *
 * Conventions implemented here (components/HTTP_API.md §1): JSON in/out,
 * GET never mutates, password redaction on reads ("" on PUT = keep stored),
 * reboots only via restart_tracker_restart() after the response flushed.
 *
 * Lifecycle (called by main, in dependency order):
 *   http_server_manager_init();
 *   api_http_init();               // registers the /api routes (buffered)
 *   ...
 *   http_server_manager_start();   // routes go live, catch-all last
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the /api routes with http_server_manager (buffered until the
 *  server starts) and the component's log descriptor. Call after
 *  http_server_manager_init(). */
esp_err_t api_http_init(void);

/** Lifecycle uniformity (§3); passive after init — both trivial. */
esp_err_t api_http_start(void);
esp_err_t api_http_stop(void);

#ifdef __cplusplus
}
#endif
