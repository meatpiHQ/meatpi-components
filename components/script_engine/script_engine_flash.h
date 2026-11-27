/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_flash.h
 * @brief Internals of ECU flashing from scripts (device side): the ISO-TP
 *        sessions and raw frames of script_engine_tp.c, the SD file access
 *        of script_engine_flash_file.c, and the run state, lock and
 *        bindings of script_engine_flash.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "berry.h"
#include "cJSON.h"
#include "esp_err.h"

#include "can_core.h"

#include "script_engine_flash_core.h"
#include "script_engine_tp_core.h"

#define SE_TP_SESSIONS 4

/* ---- ISO-TP sessions and raw frames (script_engine_tp.c) ------------------ */

/** Open a session; returns its handle (0..SE_TP_SESSIONS-1) or -1. */
int se_tp_open(uint32_t tx_id, uint32_t rx_id, bool ext,
               const se_tp_opts_t *o, uint32_t gap_us);
void se_tp_close(int h);

/** Send one PDU (single frame, or first frame + flow control + paced
 *  consecutive frames). ESP_ERR_TIMEOUT: no flow control. */
esp_err_t se_tp_send(int h, const uint8_t *msg, size_t len,
                     uint32_t timeout_ms);

/** Receive one PDU; *out points into the session's buffer until the next
 *  call. @p timeout_ms runs until the first frame, then between frames
 *  (0 looks once). ESP_ERR_TIMEOUT: nothing complete in time. */
esp_err_t se_tp_recv(int h, const uint8_t **out, size_t *len,
                     uint32_t timeout_ms);

/** Drop whatever the session has queued (a stale reply). */
void se_tp_flush(int h);

/** One raw frame. Opens the raw subscription first and drops the frames
 *  it queued before the send. */
esp_err_t se_raw_send(uint32_t id, bool ext, const uint8_t *d, uint8_t dlc);

/** The next frame whose id is in @p ids (all when n_ids is 0). A timeout
 *  of 0 looks at the queue once. */
esp_err_t se_raw_recv(const uint32_t *ids, size_t n_ids, uint32_t timeout_ms,
                      can_core_frame_t *out);

/** True once no matching frame was seen for @p window_ms; false when
 *  @p timeout_ms ran out first. */
bool se_raw_quiet(const uint32_t *ids, size_t n_ids, uint32_t window_ms,
                  uint32_t timeout_ms);

/** After every run: close every session and the raw subscription. */
void se_tp_epilogue(void);
/** Before the script stops the bus: the same, while the slots are ours. */
void se_tp_bus_down(void);

/* ---- SD files (script_engine_flash_file.c) --------------------------------- */

/** Only /sd/... paths without "..". */
bool se_file_path_ok(const char *path);
esp_err_t se_file_size(const char *path, size_t *size);
/** Read up to @p len bytes at @p off into @p buf (PSRAM is fine). */
esp_err_t se_file_read(const char *path, size_t off, uint8_t *buf, size_t len,
                       size_t *got);
/** Write (or append) @p len bytes; parent directories are created. */
esp_err_t se_file_write(const char *path, const uint8_t *d, size_t len,
                        bool append);
/** SHA-256 of a whole file, as 64 hex characters. */
esp_err_t se_file_sha256(const char *path, char hex[65]);

/* ---- the run, the lock, the bindings (script_engine_flash.c) -------------- */

/** Register the flash_* bindings into a VM. */
void se_flash_bindings_register(bvm *vm);
size_t      se_flash_binding_count(void);
/* argument helpers shared by the flash_* binding files (raise on misuse) */
const uint8_t *se_fl_bytes(bvm *vm, int idx, size_t *len);
uint32_t       se_fl_int(bvm *vm, int idx, uint32_t def);
/* an int in 0..max at idx, @p def when absent or nil, else raises @p what */
uint32_t       se_fl_range(bvm *vm, int idx, uint32_t def, uint32_t max,
                           const char *what);
const char    *se_fl_str(bvm *vm, int idx, const char *what);
/* the data bindings (script_engine_flash_bind_data.c) */
int se_fb_file_size(bvm *vm);
int se_fb_file_read(bvm *vm);
int se_fb_file_write(bvm *vm);
int se_fb_file_sha256(bvm *vm);
int se_fb_crc(bvm *vm);
int se_fb_adler32(bvm *vm);
int se_fb_sum(bvm *vm);
int se_fb_sha256(bvm *vm);
/* the run bindings (script_engine_flash_bind_run.c) */
int se_fr_abort_requested(bvm *vm);
int se_fr_state(bvm *vm);
int se_fr_phase(bvm *vm);
int se_fr_progress(bvm *vm);
int se_fr_log(bvm *vm);
int se_fr_ask(bvm *vm);
int se_fr_param(bvm *vm);
int se_fr_micros(bvm *vm);
const char *se_flash_binding_name(size_t i);

/** A run id for an async run (allocated when it is started). */
uint32_t se_flash_next_run_id(void);

/** A run begins: params (taken; may be NULL), whether it is async and its
 *  id. Only an async run resets and fills the status (phase, progress,
 *  result, params); every run counts as running for the lock. */
void se_flash_run_begin(cJSON *params, bool async, uint32_t run_id);

/** A run ended: the lock follows its outcome; sessions are closed. */
void se_flash_run_end(bool ok, const char *error);

/** Boot: read the committed record from NVS (call from start). */
void se_flash_boot(void);

se_lock_state_t se_flash_lock_state(void);
/** The lock holds (committed, failed or recovery). */
bool se_flash_lock_held(void);
/** The run in flight was started async (POST /api/scripts/run async). */
bool se_flash_run_is_async(void);

/** Stop pressed: before commit the run is killed, after it the script is
 *  asked (flash_abort_requested). Returns true when it became a request. */
bool se_flash_stop(void);

/** The runner's task, for the TX gate. */
void se_flash_set_runner(void *task_handle);

/** The status document of GET /api/scripts/status (caller frees). */
cJSON *se_flash_status(uint32_t since);

/** POST /api/scripts/answer. ESP_ERR_INVALID_STATE: no open prompt. */
esp_err_t se_flash_answer(const char *value);

/** POST /api/scripts/release. ESP_ERR_INVALID_STATE: nothing to release
 *  (or committed: a running job is never released). */
esp_err_t se_flash_release(void);

/** True while a committed run is past max_runtime_ms's reach. */
bool se_flash_budget_exempt(void);

/* what the bindings change, on the runner */
/* NVS first, then lock. ESP_ERR_NOT_SUPPORTED: not an async run;
   ESP_ERR_INVALID_STATE: a firmware update of the WiCAN or its OBD chip
   runs; ESP_ERR_NOT_ALLOWED: Stop was pressed (record erased again);
   other: the NVS write failed. In every error case nothing is locked. */
esp_err_t    se_flash_commit(const char *label);
void         se_flash_set_phase(const char *phase);
void         se_flash_set_progress(uint32_t done, uint32_t total);
void         se_flash_set_ecu_state(const char *state);
bool         se_flash_abort_requested(void);
void         se_flash_log(const char *msg);
const cJSON *se_flash_params(void);
esp_err_t    se_flash_ask(const char *text, const char *choices,
                          bool (*alive)(void *), void *ctx,
                          char *answer, size_t cap);

/* ---- the gates (script_engine_flash.c), wired by script_engine_init ------- */

/** can_manager's TX gate: while the lock holds, only the runner sends. */
bool script_engine_tx_allowed(void);
/** http_server_manager's lock gate: 423 for changes while it holds. */
bool script_engine_http_allowed(bool read_only, const char *uri);
/* script_engine_busy_reason() is public (include/script_engine.h): the
   sleep check here, the restart hold in main */
