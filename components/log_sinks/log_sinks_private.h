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
 * @file log_sinks_private.h
 * @brief Component-internal contracts between log_sinks.c /
 *        log_sinks_file.c / log_sinks_settings.c / log_sinks_cli.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "log_sinks_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sink ring sizes (PSRAM .bss). The file ring doubles as the batch
 * accumulator, so it's the largest. */
#define LS_TCP_RING_SZ  8192
#define LS_UDP_RING_SZ  4096
#define LS_WS_RING_SZ   8192
#define LS_FILE_RING_SZ 16384

#define LS_TCP_MAX_CLIENTS 2
#define LS_UDP_BATCH_MAX   1400 /* one datagram stays under a WiFi MTU  */
#define LS_WS_FRAME_MAX    1024
#define LS_FILE_CHUNK      2048 /* per-fwrite drain unit (internal buf) */

#define LS_FILE_DIR    "/sd/devlog"
#define LS_FILE_PREFIX "devlog"
#define LS_FILE_KEEP_MAX 16

/* File-buffer high-water: flush early when the ring is this full even
 * before flush_s elapses (wear rule: batch, never per-line). */
#define LS_FILE_HIWATER (LS_FILE_RING_SZ - (LS_FILE_RING_SZ / 4))

/** Boot-applied config (filled by log_sinks_settings.c on_apply). */
typedef struct
{
    bool     tcp_enabled;
    uint16_t tcp_port;
    bool     udp_enabled;
    char     udp_host[64];
    uint16_t udp_port;
    bool     ws_enabled;
    bool     file_enabled;
    uint16_t file_max_kb;
    uint8_t  file_keep;
    uint16_t file_flush_s;
} log_sinks_config_t;

const log_sinks_config_t *ls_settings_config(void);
bool ls_settings_is_configured(void);
esp_err_t ls_settings_register(void);

/* engine internals shared with the CLI / file writer */
esp_err_t log_sinks_register_cli(void);

/* file writer half (log_sinks_file.c) */
esp_err_t ls_file_start(void);           /* spawn the internal-stack writer */
void      ls_file_request_flush(void);
void      ls_file_notify(void);          /* data arrived in the file ring   */
uint32_t  ls_file_rotations(void);

/* the file ring lives in log_sinks.c next to the others; the writer
 * drains it through these locked accessors */
uint32_t ls_file_ring_pop_batch(void *dst, uint32_t budget,
                                uint32_t *lines_out);
uint32_t ls_file_ring_used(void);

/* bench/CLI line generator (runs in the caller's context) */
void ls_emit_lines(uint32_t n, uint32_t gap_ms);

#ifdef __cplusplus
}
#endif
