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
 * @file script_engine.h
 * @brief Berry scripting for WiCAN — runs stored scripts against the
 *        device bindings (uds.*, can.*, event.*, value.get, log, sleep).
 *
 * A script is Berry source on /data/scripts/<name>.be. One runs at a
 * time on a dedicated PSRAM-stack task with a runtime budget + a kill
 * switch. The bindings ARE the device API (SCRIPTING.md): no privileged
 * path, big UDS payloads go through the uds.* binding (script-owned),
 * never the event queue.
 *
 * Settings ("script_engine", reboot-to-apply): enabled, max_runtime_ms,
 * cli.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t script_engine_init(void);
esp_err_t script_engine_start(void);
esp_err_t script_engine_stop(void);

/**
 * Run Berry source synchronously (caller's context), capturing output.
 * Serialized: ESP_ERR_INVALID_STATE if a script is already running.
 * @param src   Berry source.
 * @param out   optional buffer for captured print()/error output.
 * @param cap   size of @p out.
 * @return ESP_OK on clean run, ESP_FAIL on a Berry error (message in out).
 */
esp_err_t script_engine_run(const char *src, char *out, size_t cap);

/**
 * Run a STORED script: /data/scripts/<name>.be (".be" appended if
 * missing; name restricted to [A-Za-z0-9_-]). Loads the source to a
 * PSRAM buffer and runs it like script_engine_run.
 * ESP_ERR_NOT_FOUND if the file doesn't exist.
 */
esp_err_t script_engine_run_file(const char *name, char *out, size_t cap);

/** Request the running script stop (checked at the debug hook). */
void script_engine_kill(void);

/** True while a script is executing. */
bool script_engine_busy(void);

esp_err_t script_engine_register_http(void); /* /api/scripts* */

#ifdef __cplusplus
}
#endif
