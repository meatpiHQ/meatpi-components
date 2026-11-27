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
 * @file script_engine_tp_core.h
 * @brief ISO 15765-2 framing for the flash bindings: pure (no IDF, no
 *        Berry), host-tested. The device side (script_engine_tp.c) puts
 *        these frames on the bus with its own pacing.
 *
 * Why its own framing and not can_isotp_esp: a flash script talks to
 * loaders that are not UDS servers (native protocols on arbitrary IDs,
 * extended addressing with a different byte each way), sends 1-4 KiB
 * PDUs, and needs a fixed gap between consecutive frames that it sets
 * itself. The whole exchange runs on the script runner task, which is
 * what lets the bus lock admit exactly the runner's frames.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest PDU (12-bit first-frame length). */
#define SE_TP_MAX 4095

typedef struct
{
    int16_t ae_tx;    /**< extended-address byte we prefix; -1 = none     */
    int16_t ae_rx;    /**< first byte a reply must carry; -1 = none       */
    bool    pad;      /**< pad every frame to 8 bytes                     */
    uint8_t pad_byte; /**< the padding value                              */
    uint8_t bs;       /**< block size we advertise in our flow control    */
    uint8_t stmin;    /**< STmin byte we advertise                        */
} se_tp_opts_t;

/** Defaults: normal addressing, padded with 0x00, BS 0, STmin 0. */
void se_tp_opts_default(se_tp_opts_t *o);

/**
 * The first frame of a message: a single frame when it fits, a first frame
 * otherwise. Returns the DLC (0: len is 0 or over SE_TP_MAX); @p consumed
 * is the payload bytes it carries.
 */
uint8_t se_tp_first(const se_tp_opts_t *o, const uint8_t *msg, size_t len,
                    uint8_t out[8], size_t *consumed);

/** A consecutive frame from msg[*off]; advances *off. @p sn: 0..15. */
uint8_t se_tp_consecutive(const se_tp_opts_t *o, const uint8_t *msg,
                          size_t len, size_t *off, uint8_t sn,
                          uint8_t out[8]);

/** Our flow control: continue to send, with o->bs and o->stmin. */
uint8_t se_tp_flow_control(const se_tp_opts_t *o, uint8_t out[8]);

typedef enum
{
    SE_TP_FC_CTS = 0,
    SE_TP_FC_WAIT,
    SE_TP_FC_OVERFLOW,
    SE_TP_FC_NOT_FC, /**< not a flow control frame (or the wrong AE)      */
} se_tp_fc_t;

/** Parse a received flow control; bs and stmin_us filled for CTS. */
se_tp_fc_t se_tp_parse_fc(const se_tp_opts_t *o, const uint8_t *d,
                          uint8_t dlc, uint8_t *bs, uint32_t *stmin_us);

/** STmin byte to µs: 0x00-0x7F ms, 0xF1-0xF9 100-900 µs, others 127 ms. */
uint32_t se_tp_stmin_us(uint8_t stmin);

/** Reassembly state for one session; buf is the caller's. */
typedef struct
{
    uint8_t *buf;
    size_t   cap;
    size_t   len;     /**< announced length                               */
    size_t   got;
    uint8_t  next_sn;
    uint8_t  blk;     /**< CFs taken since our last flow control          */
    bool     active;  /**< a first frame was seen, CFs outstanding        */
} se_tp_rx_t;

typedef enum
{
    SE_TP_RX_IGNORED = 0, /**< not ours (AE mismatch, a flow control)     */
    SE_TP_RX_MORE,        /**< a CF was taken, more to come               */
    SE_TP_RX_NEED_FC,     /**< a first frame, or the end of a block of
                               o->bs CFs: send our flow control           */
    SE_TP_RX_DONE,        /**< buf[0..len) holds a whole message          */
    SE_TP_RX_ERROR,       /**< bad PCI, sequence, or over cap: state reset */
} se_tp_rx_status_t;

void se_tp_rx_init(se_tp_rx_t *rx, uint8_t *buf, size_t cap);

/** Feed one received frame. */
se_tp_rx_status_t se_tp_rx_feed(se_tp_rx_t *rx, const se_tp_opts_t *o,
                                const uint8_t *d, uint8_t dlc);

#ifdef __cplusplus
}
#endif
