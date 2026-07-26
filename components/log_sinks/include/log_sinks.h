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
 * @file log_sinks.h
 * @brief External log sinks (service component): the network/file
 *        registrants log_manager deliberately doesn't own (standard §9.3,
 *        Architecture §3 — the log pipeline has no network or filesystem
 *        dependency; the sinks live HERE).
 *
 * Four sinks, each behind its own default-false settings gate:
 *  - "tcp":  a live log-tail TCP server on the device (nc <ip> 5515) —
 *            the j2534_server ownership model: own listener, own port.
 *  - "udp":  push to a configured collector host:port (one datagram per
 *            batch of lines) — the classic remote-syslog shape.
 *  - "ws":   text frames on the websocket_manager `ws_log` channel
 *            (`/ws/log`) for the web UI / browser tooling.
 *  - "file": batched, rotation-bounded plain-text log files on the SD
 *            card (/sd/devlog), wear-safe per the log_manager README
 *            recipe: accumulate in PSRAM, flush on buffer-high-water or a
 *            coarse period through an INTERNAL-stack writer (§2 corollary).
 *
 * Pipeline contract: every sink's log_manager write callback only copies
 * the line into a per-sink PSRAM record ring (drop-oldest, counted) and
 * notifies a flusher task — the log task is never blocked by a socket,
 * a slow client, or the SD card (§9.5). Two flushers: one network task
 * (PSRAM stack) for tcp/udp/ws, one file writer (internal stack).
 *
 * Sinks register with log_manager at start() and show up in
 * `GET /api/logs/status` / `PUT /api/logs/sink` automatically; those
 * runtime toggles pause/resume a RUNNING sink (ephemeral, §9.4) — engine
 * bring-up itself is reboot-to-apply via the "log_sinks" settings.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-sink pipeline counters (conservation: in == out + dropped +
 *  still-buffered; `in` counts only lines accepted while the sink had a
 *  consumer — see README). */
typedef struct
{
    bool     enabled;   /* settings gate (engine configured at boot)     */
    uint32_t in;        /* lines accepted into the sink's ring           */
    uint32_t out;       /* lines handed to the wire/file                 */
    uint32_t dropped;   /* ring evictions + laggard-client skips         */
    uint32_t buffered;  /* lines currently queued in the ring            */
    uint32_t detail;    /* tcp: connected clients; udp: send failures;
                           ws: connected clients; file: rotations        */
} log_sinks_stats_t;

typedef enum
{
    LOG_SINKS_TCP = 0,
    LOG_SINKS_UDP,
    LOG_SINKS_WS,
    LOG_SINKS_FILE,
    LOG_SINKS_COUNT
} log_sinks_id_t;

/** Register the "log_sinks" settings descriptor + log descriptor. */
esp_err_t log_sinks_init(void);

/** Bring up the gate-enabled sinks: register them with log_manager and
 *  spawn the flusher task(s). No-op (ESP_OK) when every gate is off. */
esp_err_t log_sinks_start(void);
esp_err_t log_sinks_stop(void);

/** Counters for one sink. ESP_ERR_INVALID_ARG past LOG_SINKS_COUNT. */
esp_err_t log_sinks_stats(log_sinks_id_t id, log_sinks_stats_t *out);

/** Ask the file writer to flush its buffer to the card now (event-driven
 *  write, §11-sanctioned). Returns ESP_ERR_INVALID_STATE when the file
 *  sink is not running. */
esp_err_t log_sinks_file_flush(void);

#ifdef __cplusplus
}
#endif
