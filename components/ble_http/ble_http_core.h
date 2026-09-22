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
 * @file ble_http_core.h
 * @brief The PURE tunnel core (host-tested, no IDF deps beyond esp_err /
 *        cJSON): frame codec, request-head parser, response-head builder
 *        and the request state machine, driven by an injected HTTP-side
 *        vtable and an emit callback. The wire format is documented in
 *        BLE_HTTP_PROTOCOL.md; the constants here ARE that contract.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- wire constants (BLE_HTTP_PROTOCOL.md §2) ---------------------------------- */

#define BLEH_MAGIC          0x57u  /* 'W' */
#define BLEH_VERSION        0x02u  /* v2 (2026-09-22): body-frame counter in
                                      flags, CREDIT in both directions   */
#define BLEH_HDR_SIZE       8
#define BLEH_FRAME_MAX      4096   /* payload bytes per frame               */
#define BLEH_FRAME_BUF      (BLEH_HDR_SIZE + BLEH_FRAME_MAX)

#define BLEH_T_REQ          0x01   /* app -> dev: JSON request head         */
#define BLEH_T_REQ_BODY     0x02   /* app -> dev: body bytes                */
#define BLEH_T_RSP          0x03   /* dev -> app: JSON response head        */
#define BLEH_T_RSP_BODY     0x04   /* dev -> app: body bytes                */
#define BLEH_T_ABORT        0x05   /* app -> dev: cancel the request        */
#define BLEH_T_CREDIT       0x06   /* both ways: u32 LE body bytes accepted */

#define BLEH_F_LAST         0x01   /* final body frame of a request/response*/
#define BLEH_F_CTR_SHIFT    4      /* flags[7:4]: body-frame counter mod 16 */
#define BLEH_F_CTR_MASK     0xF0u  /* REQ_BODY / RSP_BODY: 0 on the first
                                      frame of a body, +1 per frame (LAST
                                      included). A gap = a lost PDU        */
#define BLEH_CTR_FLAGS(ctr, last) \
    ((uint8_t)((((ctr) & 0x0Fu) << BLEH_F_CTR_SHIFT) | ((last) ? BLEH_F_LAST : 0)))

#define BLEH_PATH_MAX       256    /* path + query, incl. NUL               */
#define BLEH_CT_MAX         64     /* content-type, incl. NUL               */
#define BLEH_METHOD_MAX     8
#define BLEH_BODY_MAX       (8u * 1024u * 1024u) /* request body guard      */
#define BLEH_CREDIT_STEP    8192   /* CREDIT every >= this many body bytes  */
#define BLEH_CREDIT_WINDOW  16384  /* bytes the app may have unacknowledged */
#define BLEH_OUT_WINDOW     16384  /* notify mode: RSP_BODY bytes the device
                                      keeps unacknowledged by app CREDITs  */
#define BLEH_IDLE_TIMEOUT_MS 30000 /* no body frame / CREDIT this long: end */
#define BLEH_PATH_PREFIX    "/api/"

/* tunnel-generated statuses (the JSON "err" code goes with them) */
#define BLEH_S_BAD_REQUEST  400    /* incl. err "hole": a REQ_BODY counter gap */
#define BLEH_S_FORBIDDEN    403
#define BLEH_S_BUSY         429
#define BLEH_S_ABORTED      499
#define BLEH_S_UPSTREAM     502
#define BLEH_S_TIMEOUT      504

/* ---- frame codec ---------------------------------------------------------------- */

typedef struct
{
    uint8_t  type;
    uint8_t  flags;
    uint16_t seq;
    uint16_t len;
} bleh_hdr_t;

/** Parse 8 header bytes. False on bad magic/version/type or len > max. */
bool   bleh_hdr_parse(const uint8_t *b, bleh_hdr_t *h);
void   bleh_hdr_build(uint8_t *b, uint8_t type, uint8_t flags, uint16_t seq,
                      uint16_t len);

/* ---- request / response heads ------------------------------------------------------ */

typedef struct
{
    char     method[BLEH_METHOD_MAX];
    char     path[BLEH_PATH_MAX];
    char     ct[BLEH_CT_MAX];
    uint32_t len;               /* body bytes that follow                */
} bleh_req_t;

/** Parse a REQ head (JSON, n bytes, not NUL-terminated). On failure the
 *  tunnel status + error code to answer with are returned. */
bool   bleh_req_parse(const char *json, size_t n, bleh_req_t *out,
                      int *status, const char **err);

/** Build a RSP head: {"s":200,"ct":"...","len":N} or, with err != NULL,
 *  {"s":429,"err":"busy"}. len < 0 = unknown (chunked). Returns bytes. */
size_t bleh_rsp_head(char *dst, size_t cap, int status, const char *ct,
                     int64_t len, const char *err);

/* ---- the HTTP side (injected: esp_http_client on target, a fake on the host) ------ */

typedef struct
{
    /** Open the upstream request (headers sent, body may follow). */
    esp_err_t (*open)(void *ctx, const bleh_req_t *req);
    /** Forward body bytes. */
    esp_err_t (*write)(void *ctx, const uint8_t *data, size_t len);
    /** Complete the request, read the response head. len < 0 = unknown. */
    esp_err_t (*fetch)(void *ctx, int *status, char *ct, size_t ct_cap,
                       int64_t *len);
    /** Read response body: >0 bytes, 0 = complete, <0 = error. */
    int       (*read)(void *ctx, uint8_t *dst, size_t cap);
    /** Release the upstream request (idempotent). */
    void      (*close)(void *ctx);
} bleh_ops_t;

/** Send one complete frame (header + payload, n bytes). False = link
 *  gone (the core aborts the request). */
typedef bool (*bleh_emit_fn)(void *arg, const uint8_t *frame, size_t n);

/* ---- the core ------------------------------------------------------------------------ */

typedef enum
{
    BLEH_ST_IDLE = 0,
    BLEH_ST_BODY,               /* REQ accepted, body frames expected    */
    BLEH_ST_RESPONDING,         /* RSP head sent, body streams via pump  */
} bleh_state_t;

typedef struct
{
    uint32_t requests, responses, errors, resync, aborts, timeouts;
    uint32_t frames_rx, frames_tx, bytes_in, bytes_out;
    uint32_t holes;             /* REQ_BODY counter gaps (lost app PDUs) */
    uint32_t credits_rx;        /* CREDIT frames from the app            */
    uint32_t credit_stalls;     /* pump paused on the OUT window         */
    int      last_status;
} bleh_counters_t;

typedef struct
{
    const bleh_ops_t *ops;
    void             *ops_ctx;
    bleh_emit_fn      emit;
    void             *emit_arg;

    uint8_t          *rx;       /* BLEH_FRAME_BUF: frame reassembly       */
    size_t            rx_len;
    uint8_t          *tx;       /* BLEH_FRAME_BUF: outgoing frame staging */

    bleh_state_t      state;
    bleh_req_t        req;
    uint16_t          seq;      /* of the active request                  */
    uint32_t          received;
    uint32_t          last_credit;
    uint32_t          last_activity_ms;
    bool              upstream_open;
    uint8_t           req_ctr;  /* next REQ_BODY counter expected         */

    /* the response stream (BLEH_ST_RESPONDING) */
    uint32_t          out_sent;    /* RSP_BODY payload bytes emitted      */
    uint32_t          out_acked;   /* the app's last CREDIT               */
    uint32_t          out_window;  /* 0 = no app credits (indications)    */
    uint16_t          out_frame;   /* RSP_BODY payload cap per frame      */
    uint8_t           rsp_ctr;     /* next RSP_BODY counter               */
    bool              link_notify; /* sampled at the REQ                  */
    uint16_t          link_pdu;
    bool              out_stalled;  /* pump paused on the window (counted) */

    bleh_counters_t   ctr;
} bleh_core_t;

/** rx / tx are caller-owned BLEH_FRAME_BUF-byte buffers (PSRAM). */
void   bleh_core_init(bleh_core_t *c, const bleh_ops_t *ops, void *ops_ctx,
                      bleh_emit_fn emit, void *emit_arg, uint8_t *rx,
                      uint8_t *tx);

/** The link the next request will stream on. notify = the central chose
 *  notifications on the OUT characteristic: RSP_BODY frames are then
 *  PDU-aligned (payload <= pdu_payload - 8, one frame per notification so
 *  a lost PDU is a lost whole frame and the counter shows it) and the
 *  device keeps at most BLEH_OUT_WINDOW bytes unacknowledged by the app's
 *  CREDITs. Indications: 4096-byte frames, no app credits (each PDU is
 *  confirmed). Sampled when a REQ arrives. */
void   bleh_core_set_link(bleh_core_t *c, bool notify, uint16_t pdu_payload);

/** Feed bytes from the channel; complete frames are handled inline
 *  (which may block in ops->write/fetch and emit). A response body is NOT
 *  streamed here: bleh_core_pump() does that, so CREDIT / ABORT frames
 *  keep flowing in while a download runs. */
void   bleh_core_rx(bleh_core_t *c, const uint8_t *data, size_t len,
                    uint32_t now_ms);

/** Stream up to a few RSP_BODY frames of the active response, honouring
 *  the OUT window. Returns true when the response is still open and has
 *  more to send once credits arrive or now (call again soon), false when
 *  idle or blocked on the window. May block in ops->read and emit. */
bool   bleh_core_pump(bleh_core_t *c, uint32_t now_ms);

/** Periodic: enforces the body idle timeout (upload without frames,
 *  download without CREDITs). */
void   bleh_core_tick(bleh_core_t *c, uint32_t now_ms);

/** The link dropped: release the upstream request, reset the parser. */
void   bleh_core_link_down(bleh_core_t *c);

#ifdef __cplusplus
}
#endif
