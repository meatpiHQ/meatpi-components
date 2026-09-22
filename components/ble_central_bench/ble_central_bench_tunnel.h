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
 * @file ble_central_bench_tunnel.h
 * @brief The APP side of `ble_http/BLE_HTTP_PROTOCOL.md`, pure C (no IDF):
 *        frame header build/parse, the JSON request head, and a client
 *        state machine fed with the bytes that arrive on the OUT
 *        characteristic. It tells the caller what to send (frames) and
 *        what arrived (response head, body bytes, credits). Host-tested.
 *
 *        Wire (8-byte LE header): 57 01 type flags seq(u16) len(u16)
 *        types: 1 REQ 2 REQ_BODY 3 RSP 4 RSP_BODY 5 ABORT 6 CREDIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BCBT_HDR          8
#define BCBT_VERSION      2             /* protocol v2 (counter + credits)   */
#define BCBT_FRAME_MAX    4096          /* payload per frame                 */
#define BCBT_CREDIT_WIN   16384         /* bytes the app may have unacked    */
#define BCBT_CREDIT_STEP  4096          /* notify mode: CREDIT the device
                                           every >= this many body bytes     */
#define BCBT_HEAD_MAX     320           /* JSON request head cap             */

enum
{
    BCBT_T_REQ = 1, BCBT_T_REQ_BODY, BCBT_T_RSP, BCBT_T_RSP_BODY,
    BCBT_T_ABORT, BCBT_T_CREDIT,
};

#define BCBT_F_LAST      0x01
#define BCBT_F_CTR_SHIFT 4              /* flags[7:4]: body-frame counter    */
#define BCBT_CTR_FLAGS(ctr, last) \
    ((uint8_t)((((ctr) & 0x0Fu) << BCBT_F_CTR_SHIFT) | ((last) ? BCBT_F_LAST : 0)))

/** Build a frame header into @p b (BCBT_HDR bytes). */
void bcbt_hdr_build(uint8_t *b, uint8_t type, uint8_t flags, uint16_t seq,
                    uint16_t len);

/** Parse a header; false when magic/version/type/len are not plausible. */
bool bcbt_hdr_parse(const uint8_t *b, uint8_t *type, uint8_t *flags,
                    uint16_t *seq, uint16_t *len);

/** The JSON request head: {"m":..,"p":..,"ct":..,"len":N}. Returns the
 *  length written (0 when it does not fit). ct may be NULL. */
size_t bcbt_req_head(char *dst, size_t cap, const char *method,
                     const char *path, const char *ct, uint32_t body_len);

/** Minimal parse of the response head: status and len (-1 unknown). */
bool bcbt_rsp_head_parse(const char *json, size_t n, int *status,
                         int64_t *len);

/* ---- the client state machine ------------------------------------------------ */

typedef enum
{
    BCBT_EV_NONE = 0,
    BCBT_EV_CREDIT,         /* credit: bytes accepted so far               */
    BCBT_EV_RSP,            /* response head: status, len                  */
    BCBT_EV_BODY,           /* body bytes (ptr/len valid until next feed)  */
    BCBT_EV_DONE,           /* final body frame seen (or empty response)   */
    BCBT_EV_STRAY,          /* a frame for another seq / unexpected type   */
    BCBT_EV_HOLE,           /* RSP_BODY counter gap: a PDU was lost        */
} bcbt_event_t;

typedef struct
{
    uint16_t seq;           /* the request in flight                       */
    uint8_t  rx[BCBT_HDR + BCBT_FRAME_MAX];
    size_t   rx_len;
    uint32_t credited;      /* last CREDIT value                           */
    uint32_t resync;        /* bad headers skipped                         */
    int      status;        /* response status once seen                   */
    int64_t  rsp_len;
    bool     rsp_seen;
    bool     done;
    uint8_t  body_ctr;      /* next RSP_BODY counter expected              */
    uint32_t holes;         /* counter gaps seen                           */
    uint32_t body_rx;       /* RSP_BODY payload bytes received             */
    uint32_t credit_sent;   /* body_rx value of the last CREDIT we sent    */
} bcbt_client_t;

void bcbt_client_begin(bcbt_client_t *c, uint16_t seq);

/** Feed bytes from the OUT characteristic. Calls @p on_event for every
 *  complete frame (BODY events carry the payload). Returns the number of
 *  complete frames consumed. */
typedef void (*bcbt_event_fn)(void *arg, bcbt_event_t ev, const uint8_t *p,
                              size_t n, uint32_t value);
size_t bcbt_client_feed(bcbt_client_t *c, const uint8_t *data, size_t n,
                        bcbt_event_fn on_event, void *arg);

/** May the app send @p sent_total bytes of body given the last credit? */
bool bcbt_client_may_send(const bcbt_client_t *c, uint32_t sent_total);

/** Notify mode: true when the device is owed a CREDIT (>= step bytes of
 *  body since the last one, or the response is done with bytes pending). */
bool bcbt_client_credit_due(const bcbt_client_t *c, uint32_t step);

/** Build the CREDIT frame for what was received so far; marks it sent.
 *  Returns the frame size (BCBT_HDR + 4). */
size_t bcbt_client_credit_frame(bcbt_client_t *c, uint8_t *dst);

#ifdef __cplusplus
}
#endif
