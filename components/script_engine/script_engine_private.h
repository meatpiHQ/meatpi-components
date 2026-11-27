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

/** Internals shared across the script_engine .c files. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "berry.h"
#include "esp_err.h"

#include "event_manager.h"

#define SE_SCRIPTS_DIR "/data/scripts"

/* Register all device bindings (uds/can/event/value/log/sleep) into a VM. */
void se_bindings_register(bvm *vm);
/* Every binding documented in script_engine_doc.c and vice versa (logs
 * the drift; called once at init). */
bool se_bindings_selfcheck(void);
/** Install the uds_manager-backed obd.* port (bind.c; call once at init). */
void se_obd_port_install(void);

/* [A-Za-z0-9_.-], <=40 chars, no "..": safe as a /data/scripts entry. */
bool se_script_name_ok(const char *name);

/* Set/clear the trigger event exposed to the NEXT run as evt_* globals
 * (NULL clears). The events glue sets it before an event-triggered run. */
void se_set_trigger(const em_event_t *ev);

/* Register the `script.run` event action (script_engine_events.c). */
void se_events_register(void);

/* Cooperative budget/kill check: bindings call it at every I/O point. */
void se_check_budget(bvm *vm);

/* A CAN id argument: an integer, or a hex string (script_engine_bind.c). */
uint32_t se_bind_id_arg(bvm *vm, int idx);

/* The bindings of script_engine_bind_can.c, listed in bind.c's table, and
 * the globals they read and set (can_rx_*, can_send_error, HIGH, LOW,
 * CAN_TX, CAN_RX). */
int se_b_can_send(bvm *vm);
int se_b_can_start(bvm *vm);
int se_b_can_stop(bvm *vm);
int se_b_can_rx(bvm *vm);
int se_b_digital_write(bvm *vm);
int se_b_digital_pwm(bvm *vm);
void se_bind_can_globals(bvm *vm);

esp_err_t script_engine_register_cli(void);

/* ---- settings (script_engine_settings.c) ------------------------------------ */

/** Register the "script_engine" descriptor with settings_manager. */
esp_err_t se_settings_register(void);

bool     se_settings_enabled(void);        /* scripts may run at all         */
uint32_t se_settings_max_runtime_ms(void); /* per-run budget (0 = unlimited) */
bool     se_settings_allow_reflash(void);  /* UDS 0x34-37 + transfer_file     */
bool     se_settings_exclusive(void);      /* scripts hold the bus (autopid off) */

/* the exclusive-bus hold (script_engine_bind.c): taken at a script's first
 * ECU access when the setting is on; the runner releases it after EVERY run
 * next to se_obd_autorelease(). */
void se_bus_hold_release(void);
/* take it (idempotent): the flash bindings' first transmission */
void se_bus_hold(void);
/* raise when an ECU flash holds the lock (the non-flash bus bindings) */
void se_refuse_when_locked(bvm *vm);
/* Stop was pressed (the kill switch the bindings poll) */
bool se_run_killed(void);

/* An async run (script_engine.c): takes @p src (heap, freed by the runner)
 * and @p params (may be NULL); @p run_id gets its number for the status.
 * ESP_ERR_INVALID_STATE: busy or disabled. The only way to run while an
 * ECU flash holds the lock (a recovery run, admitted by the HTTP gate). */
struct cJSON;
esp_err_t   script_engine_run_async(char *src, struct cJSON *params,
                                    uint32_t *run_id);
/* what the last async run printed (live while it runs) */
const char *se_async_output(void);
/* /data/scripts/<name>[.be] into a fresh PSRAM buffer (caller frees) */
esp_err_t   se_load_stored(const char *name, char **src);
/* POST /api/scripts/run with "async": true (script_engine_http_flash.c);
 * takes @p root */
esp_err_t   se_http_run_async(void *req, struct cJSON *root);
/* GET status, POST answer and release (script_engine_http_flash.c) */
esp_err_t   se_flash_register_http(void);

/* After every run: drop a can_rx subscription, release GPIO, restart CAN
 * if the script called can_stop(). */
void se_script_epilogue(void);
bool     se_settings_is_configured(void);  /* boot apply ran (standard §4.3) */

/* Provided by the berry component's ESP port. */
void berry_port_set_capture(char *buf, size_t cap);
