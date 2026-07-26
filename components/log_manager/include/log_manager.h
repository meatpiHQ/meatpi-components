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
 * @file log_manager.h
 * @brief WiCAN Debug Log Manager (core component, Coding Standard §9).
 *
 * Owns the log pipeline: it installs itself via esp_log_set_vprintf(), so
 * every component (and IDF internals) keeps calling plain ESP_LOGx — no
 * wrapper macros, no call-site migration. Captured lines flow through a
 * fixed-depth PSRAM queue drained by a dedicated log task into **sinks**.
 *
 * Routing is fully pluggable (§9.3): a sink is just {name, write(line,len)}.
 * Built in: "console" (UART, always available from init) and "ring" — a
 * PSRAM `.noinit` ring buffer that SURVIVES warm resets (same
 * magic/version/CRC + cache-msync pattern as restart_tracker), so the last
 * ~16 KiB of logs are readable after a crash with ZERO flash wear.
 * TCP / UDP / WebSocket / file sinks register from outside; the manager has
 * no network or filesystem dependency by design (Architecture §3). A file
 * sink MUST batch + rate-limit its flushes and hand writes to an
 * internal-stack task (§2 corollary; see README for the wear-safe recipe).
 *
 * Pipeline rules (§9.5): producers never block — if the queue is full the
 * oldest entry is dropped and a counter increments ("dropped N" is emitted
 * when pressure clears). A sink's write runs in the log task only, so a slow
 * sink can never stall a producer. No logging from ISRs.
 *
 * Boot order (§9.6): log_manager_init() is the FIRST init in main. Its
 * settings descriptor is registered later via log_manager_register_settings()
 * (after settings_manager_init) and applies at boot like all settings;
 * log_manager_set_level() is the sanctioned ephemeral runtime knob.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-component registration (§9.2). @c name must equal the TAG (§10). */
typedef struct
{
    const char     *name;
    esp_log_level_t default_level;
} log_descriptor_t;

/** A log sink (§9.3). @c write runs in the log task's context ONLY. */
typedef struct
{
    const char *name;
    esp_err_t (*write)(const char *line, size_t len);
} log_sink_t;

/** Install the vprintf hook, adopt the PSRAM ring, bring the console sink
 *  up. FIRST init in main; lines are routed synchronously until start(). */
esp_err_t log_manager_init(void);

/** Spawn the log task; switch to queued (wait-free producer) routing. */
esp_err_t log_manager_start(void);

/** Stop the task (drains first); fall back to synchronous console+ring. */
esp_err_t log_manager_stop(void);

/** Register this component's settings descriptor ("log_manager") — call
 *  after settings_manager_init(), before settings_manager_start(). */
esp_err_t log_manager_register_settings(void);

/* ---- component level table (§9.2) ----------------------------------------- */

/** Register a component; applies @c default_level for its TAG. */
esp_err_t log_manager_register(const log_descriptor_t *desc);

/** Runtime, EPHEMERAL level change (resets to defaults on reboot). */
esp_err_t log_manager_set_level(const char *name, esp_log_level_t level);

/** Cumulative E/W line counts since boot (health surface: a clean boot
 *  logs ZERO errors — the bench asserts it; every silent degradation is
 *  an ESP_LOGE somebody scrolled past). Either pointer may be NULL. */
void log_manager_health(uint32_t *errors, uint32_t *warnings);

/* ---- sinks (§9.3) ---------------------------------------------------------- */

/** Register a sink (struct must stay valid forever). Enabled by default. */
esp_err_t log_manager_add_sink(const log_sink_t *sink);

/** Enable/disable a sink by name at runtime (ephemeral). */
esp_err_t log_manager_sink_set_enabled(const char *name, bool enabled);

/** Enumerate registered sinks by index (for status transports).
 *  ESP_ERR_NOT_FOUND past the end; either out pointer may be NULL. */
esp_err_t log_manager_sink_get(size_t index, const char **name, bool *enabled);

/** Total lines dropped by backpressure since boot. */
uint32_t log_manager_dropped_count(void);

/** Sink-registry occupancy (§12 — cross-component registrants since
 *  log_sinks; main wires this into the WICAN CAPS line). Either pointer
 *  may be NULL. */
void log_manager_sinks_capacity(size_t *used, size_t *cap);

/* ---- PSRAM crash ring ------------------------------------------------------- */

/**
 * Copy the most recent ring content (chronological, includes lines from
 * BEFORE the last warm reset) into @p out. @p out_written (nullable) gets the
 * copied byte count. ESP_ERR_INVALID_STATE if the ring is unavailable.
 */
esp_err_t log_manager_ring_read(char *out, size_t out_len, size_t *out_written);

/** Discard the ring content (e.g. after a transport uploaded it). */
esp_err_t log_manager_ring_clear(void);

#ifdef __cplusplus
}
#endif
