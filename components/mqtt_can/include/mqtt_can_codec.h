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
 * @file mqtt_can_codec.h
 * @brief PURE legacy-JSON CAN⇄MQTT codec (host-tested; TASK_mqtt_can.md).
 *
 * Byte-parity with the legacy contract (legacy main/mqtt.c):
 *   rx: {"bus":"0","type":"rx","ts":1234,"frame":[{"id":123,"dlc":8,
 *        "rtr":false,"extd":false,"data":[1,...,8]},...]}
 *   tx: {"bus":0,"type":"tx","frame":[...]}   (parsed, "ts" ignored)
 *
 * Sizing: everything lives inside the translator ctx (BM_CTX_MAX 4096;
 * NEVER on the 4 KB pump stack — the 2026-07-22 stack-audit lesson).
 * Worst-case frame JSON ≈ 91 B → 24-frame batch ≈ 2.2 KB.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "can_core.h" /* can_core_frame_t */

#ifdef __cplusplus
extern "C" {
#endif

#define MC_BATCH_MAX 24   /* frames per publish (settings cap)          */
#define MC_JSON_MAX  2304 /* worst-case 24-frame batch + wrapper        */
#define MC_TXBUF_MAX 1024 /* inbound tx-object reassembly cap           */

/* ---- rx side: frame batcher -> legacy JSON --------------------------------- */

typedef struct
{
    uint16_t n;
    uint16_t limit;                 /* flush threshold (1..MC_BATCH_MAX) */
    uint32_t dropped;               /* frames beyond MC_BATCH_MAX        */
    can_core_frame_t frames[MC_BATCH_MAX];
} mc_batch_t;

void mc_batch_init(mc_batch_t *b, uint16_t batch_frames);

/** Unpack a can_frame_wire chunk (concatenated frames) into the batch.
 *  Frames past MC_BATCH_MAX are dropped AND counted (b->dropped).
 *  @return frames added. */
int mc_batch_add_chunk(mc_batch_t *b, const uint8_t *chunk, size_t len);

/** True when the configured limit is reached (caller emits). */
bool mc_batch_ready(const mc_batch_t *b);

/** Serialize + DRAIN the batch into legacy rx JSON. @return bytes
 *  written, 0 when the batch is empty (nothing emitted). */
size_t mc_batch_json(mc_batch_t *b, int64_t ts_ms, char *out, size_t cap);

/* ---- tx side: stream reassembler + parser ---------------------------------- */

/** Extracts complete top-level JSON objects from a fragmented byte
 *  stream (brace depth, string/escape aware). An object longer than
 *  MC_TXBUF_MAX is discarded to its closing boundary and counted. */
typedef struct
{
    uint16_t len;
    int16_t  depth;
    bool     in_str;
    bool     esc;
    bool     overflow;
    uint32_t oversize;              /* objects discarded for length      */
    char     buf[MC_TXBUF_MAX];
} mc_txasm_t;

typedef void (*mc_obj_cb_t)(void *arg, const char *json, size_t len);

void mc_txasm_init(mc_txasm_t *a);

/** Feed bytes; @p cb fires once per COMPLETE object. @return objects
 *  completed in this call. */
int mc_txasm_feed(mc_txasm_t *a, const uint8_t *in, size_t len,
                  mc_obj_cb_t cb, void *arg);

/** Parse one legacy tx object into frames. Tolerant of "ts" and of
 *  "bus" as number or string; requires "type":"tx" and a "frame"
 *  array. @return frame count (0 allowed), or -1 malformed. */
int mc_tx_parse(const char *json, size_t len, can_core_frame_t *out,
                size_t max);

#ifdef __cplusplus
}
#endif
