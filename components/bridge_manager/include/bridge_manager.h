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
 * @file bridge_manager.h
 * @brief WiCAN data-path pump (service component).
 *
 * Moves chunks between any two registered endpoints — OBD chip ↔ TCP,
 * CAN ↔ UDP, USB ↔ OBD, and any future pairing — with no per-pairing code.
 * Three-piece model (TASK_bridge_manager.md §2, decided):
 *
 *  - ENDPOINT:   anything that can source/sink chunks. Registered by name via
 *                a descriptor by thin glue or main (providers like obd_chip /
 *                socket_manager never depend on this component).
 *  - TRANSLATOR: optional framing codec between the two sides. External
 *                components (translator_slcan, …) register codecs; only "raw"
 *                (byte-transparent) is built in.
 *  - BRIDGE:     one settings-configured pairing {name, a, b, translator,
 *                enabled}. Built once at start() from the boot-applied
 *                settings (standard §4.2 reboot-to-apply — the spec's "live
 *                re-apply" is superseded by the standard, which wins).
 *
 * Chunk convention: {uint16_t len; uint8_t data[128]} — identical layout to
 * obd_chip's obd_chunk_t and socket_manager's socket_chunk_t, so their
 * subscribe/send APIs wrap into endpoints without copies or adaptation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_MANAGER_MAX_BRIDGES     6 /* 4→6 2026-07-20: sat at 4/4
                                            (3 obd defaults + br_cli left
                                            no room for a bench br_elm0
                                            without evicting br_usb_obd);
                                            ~13 KB PSRAM + ~0.5 KB
                                            internal per slot */
#define BRIDGE_MANAGER_MAX_ENDPOINTS   24 /* 8 was outgrown by main's glue;
                                             16 hit 15/16 by 2026-07-19
                                             (7 fixed + 4 sock + 3 ws +
                                             acm) — one more jack away
                                             from silent bridge drops */
#define BRIDGE_MANAGER_MAX_TRANSLATORS 8 /* 4 sat at 3/4 (slcan/realdash/
                                            gvret) — the very first
                                            registry_headroom fault the
                                            2026-07-19 health net latched */
#define BRIDGE_MANAGER_CHUNK_SIZE      128

typedef struct
{
    uint16_t len;
    uint8_t  data[BRIDGE_MANAGER_CHUNK_SIZE];
} bridge_chunk_t;

/** An endpoint: chunk source/sink. All pointers must live forever. */
typedef struct
{
    const char *name;                                 /* "obd", "tcp0", ... */
    esp_err_t (*send)(const uint8_t *data, size_t len);
    esp_err_t (*subscribe)(QueueHandle_t q);          /* RX starts flowing  */
    esp_err_t (*unsubscribe)(QueueHandle_t q);
    /** RX is a true fan-out: every subscribed queue receives a full COPY
     *  of the stream (obd_chip's subscriber model), so the endpoint is
     *  exempt from the single-consumer rule and may sit in several
     *  enabled bridges at once. Leave false for single-stream providers
     *  (their subscribe() must refuse a second queue). */
    bool multi_consumer;
} bridge_endpoint_t;

/**
 * A translator: stateful stream codec (bytes arrive fragmented across
 * chunks, so each bridge direction owns a reassembly ctx of ctx_size bytes,
 * allocated from the manager's static PSRAM pool and prepared by ctx_init).
 * decode/encode are PURE — no I/O, no globals; everything lives in ctx. One
 * input may emit zero, one, or many output frames via the sink callback.
 * Direction convention: a→b chunks run through decode, b→a through encode.
 */
typedef esp_err_t (*bridge_sink_fn_t)(void *arg, const uint8_t *out,
                                      size_t out_len);

/**
 * Reply channel (opt-in via wants_reply). Some protocols (GVRET's
 * TIME_SYNC/GET_DEV_INFO/…) must answer the client on the SAME side the
 * command arrived — the pure decode/encode sink only reaches the FAR side.
 * A translator that sets wants_reply=true makes the FIRST bytes of its ctx a
 * bridge_reply_hdr_t; the pump fills it (after ctx_init) with a sink that
 * writes back to the near endpoint. The codec calls hdr->reply(hdr->reply_arg,
 * bytes, len) to answer. Codecs with wants_reply=false (raw, slcan, realdash)
 * are unaffected and need no header.
 */
typedef struct
{
    bridge_sink_fn_t reply;
    void            *reply_arg;
} bridge_reply_hdr_t;

typedef struct
{
    const char *name;                    /* "slcan", "gvret" — settings key */
    size_t      ctx_size;                /* per-direction reassembly state  */
    bool        wants_reply;             /* ctx starts with bridge_reply_hdr_t */
    esp_err_t (*ctx_init)(void *ctx);
    esp_err_t (*decode)(void *ctx, const uint8_t *in, size_t len,
                        bridge_sink_fn_t sink, void *sink_arg);
    esp_err_t (*encode)(void *ctx, const uint8_t *in, size_t len,
                        bridge_sink_fn_t sink, void *sink_arg);

    /** OPTIONAL periodic flush for ACCUMULATING codecs (canmqtt
     *  batching): when flush != NULL, the pump calls it per direction
     *  whenever >= flush_ms elapsed since that direction's last
     *  decode/encode/flush — on the pump's own <=100 ms wakeups, so
     *  the real period is flush_ms rounded up to the next wakeup. The
     *  sink is the SAME destination the direction normally writes to.
     *  A codec with nothing pending must emit nothing. flush == NULL
     *  (every pre-2026-07-22 translator) = old behavior exactly. */
    uint32_t  flush_ms;
    esp_err_t (*flush)(void *ctx, bridge_sink_fn_t sink, void *sink_arg);
} bridge_translator_t;

typedef struct
{
    uint32_t a2b_chunks;   /* chunks pumped a -> b                        */
    uint32_t b2a_chunks;
    uint32_t a2b_bytes;
    uint32_t b2a_bytes;
    uint32_t send_errors;  /* endpoint send() failures (either direction) */
    uint32_t codec_errors; /* translator decode/encode failures           */
} bridge_stats_t;

/** Register settings ("bridge_manager") + log descriptors. */
esp_err_t bridge_manager_init(void);

/** Build and start the enabled bridges from the boot-applied settings.
 *  Unknown endpoint/translator names fail validation earlier (on_validate);
 *  a name that validated but was never registered fails here. */
esp_err_t bridge_manager_start(void);

/** Tear down every bridge (unsubscribe, stop pumps). */
esp_err_t bridge_manager_stop(void);

/** Registration — pre-start only; tables are read lock-free afterwards. */
esp_err_t bridge_manager_register_endpoint(const bridge_endpoint_t *ep);
esp_err_t bridge_manager_register_translator(const bridge_translator_t *tr);

/** Per-bridge counters by configured bridge name. */
esp_err_t bridge_manager_stats(const char *bridge_name, bridge_stats_t *out);

/** Registry occupancy for the health surface (`WICAN CAPS` + bench
 *  headroom assertion). Any pointer may be NULL. */
void bridge_manager_capacity(int *eps_used, int *eps_cap,
                             int *trs_used, int *trs_cap);

#ifdef __cplusplus
}
#endif
