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
 * @file j2534_proto.h
 * @brief SAE J2534 constants + the WiCAN PassThru wire-protocol codec.
 *        PURE (no IDF types) — host-tested. This is the contract the
 *        companion Windows DLL and the device server both implement.
 *        See TASK_j2534_server.md §4.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- J2534 protocol IDs (SAE J2534-1) — WiCAN supports CAN + ISO15765 -- */
enum {
    J2534_PROT_J1850VPW  = 1,
    J2534_PROT_J1850PWM  = 2,
    J2534_PROT_ISO9141   = 3,
    J2534_PROT_ISO14230  = 4,
    J2534_PROT_CAN       = 5,   /* supported */
    J2534_PROT_ISO15765  = 6,   /* supported (ISO-TP) */
    J2534_PROT_SCI_A_ENGINE = 7,
};

/* ---- J2534 return/status codes ---------------------------------------- */
enum {
    J2534_STATUS_NOERROR              = 0x00,
    J2534_ERR_NOT_SUPPORTED           = 0x01,
    J2534_ERR_INVALID_CHANNEL_ID      = 0x02,
    J2534_ERR_INVALID_PROTOCOL_ID     = 0x03,
    J2534_ERR_NULL_PARAMETER          = 0x04,
    J2534_ERR_INVALID_FLAGS           = 0x06,
    J2534_ERR_FAILED                  = 0x07,
    J2534_ERR_DEVICE_NOT_CONNECTED    = 0x08,
    J2534_ERR_TIMEOUT                 = 0x09,
    J2534_ERR_INVALID_MSG             = 0x0A,
    J2534_ERR_BUFFER_EMPTY            = 0x10,
    J2534_ERR_BUFFER_FULL             = 0x11,
    J2534_ERR_BUFFER_OVERFLOW         = 0x12,
    J2534_ERR_MSG_PROTOCOL_ID         = 0x14,
    J2534_ERR_INVALID_FILTER_ID       = 0x15,
    J2534_ERR_NO_FLOW_CONTROL         = 0x16,
    J2534_ERR_NOT_UNIQUE              = 0x17,
    J2534_ERR_INVALID_BAUDRATE        = 0x18,
    J2534_ERR_INVALID_DEVICE_ID       = 0x19,
    J2534_ERR_DEVICE_IN_USE           = 0x1A, /* WiCAN: second tester */
};

/* ---- filter types ----------------------------------------------------- */
enum {
    J2534_PASS_FILTER          = 1,
    J2534_BLOCK_FILTER         = 2,
    J2534_FLOW_CONTROL_FILTER  = 3, /* ISO15765 reassembly */
};

/* ---- TxFlags / RxStatus bits ------------------------------------------ */
enum {
    J2534_TX_ISO15765_FRAME_PAD = 0x00000040,
    J2534_TX_CAN_29BIT_ID       = 0x00000100,
    J2534_TX_WAIT_P3_MIN_ONLY   = 0x00000200,
};
enum {
    J2534_RX_TX_MSG_TYPE           = 0x00000001, /* echo of a tx */
    J2534_RX_START_OF_MESSAGE      = 0x00000002, /* ISO15765 FF indication */
    J2534_RX_ISO15765_PADDING_ERR  = 0x00000010,
    J2534_RX_ISO15765_ADDR_TYPE    = 0x00000080,
    J2534_RX_CAN_29BIT_ID          = 0x00000100,
};

/* ---- Ioctl IDs + SET_CONFIG parameter IDs ----------------------------- */
enum {
    J2534_IOCTL_GET_CONFIG               = 0x01,
    J2534_IOCTL_SET_CONFIG               = 0x02,
    J2534_IOCTL_READ_VBATT               = 0x03,
    J2534_IOCTL_CLEAR_TX_BUFFER          = 0x07,
    J2534_IOCTL_CLEAR_RX_BUFFER          = 0x08,
    J2534_IOCTL_CLEAR_PERIODIC_MSGS      = 0x09,
    J2534_IOCTL_CLEAR_MSG_FILTERS        = 0x0A,
};
enum {
    J2534_CFG_DATA_RATE      = 0x01,
    J2534_CFG_LOOPBACK       = 0x03,
    J2534_CFG_ISO15765_BS    = 0x1E,
    J2534_CFG_ISO15765_STMIN = 0x1F,
    J2534_CFG_BS_TX          = 0x22,
    J2534_CFG_STMIN_TX       = 0x23,
};

/* ---- wire framing ----------------------------------------------------- */
#define J2534_MAGIC        0x4A35u   /* "J5" */
#define J2534_WIRE_VERSION 1
#define J2534_HDR_SIZE     12
#define J2534_MAX_DATA     4128       /* SAE PASSTHRU_MSG data cap */

/* message types (TASK §4) */
enum {
    J2534_MT_HELLO          = 0x01,
    J2534_MT_OPEN           = 0x02,
    J2534_MT_CLOSE          = 0x03,
    J2534_MT_CONNECT        = 0x04,
    J2534_MT_DISCONNECT     = 0x05,
    J2534_MT_WRITE_MSGS     = 0x10,
    J2534_MT_START_FILTER   = 0x11,
    J2534_MT_STOP_FILTER    = 0x12,
    J2534_MT_START_PERIODIC = 0x13,
    J2534_MT_STOP_PERIODIC  = 0x14,
    J2534_MT_IOCTL          = 0x15,
    J2534_MT_ACK            = 0x80,
    J2534_MT_RX_MSG         = 0x81,
    J2534_MT_EVENT          = 0x82,
};

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint16_t channel;
    uint32_t length;   /* payload bytes after the header */
} j2534_hdr_t;

/** A PassThru message, device-side representation. */
typedef struct {
    uint32_t protocol_id;
    uint32_t rx_status;
    uint32_t tx_flags;
    uint32_t timestamp;      /* µs */
    uint32_t extra_data_index;
    uint32_t data_size;
    uint8_t  data[J2534_MAX_DATA];
} j2534_msg_t;

/**
 * Encode a header into @p buf (must be >= J2534_HDR_SIZE). Always writes
 * magic/version. Returns J2534_HDR_SIZE.
 */
size_t j2534_hdr_encode(uint8_t *buf, uint8_t type, uint16_t seq,
                        uint16_t channel, uint32_t length);

/**
 * Decode a header from @p buf of @p len bytes. Returns true and fills
 * @p out only when len >= J2534_HDR_SIZE, magic matches, and version is
 * accepted. Does NOT require the payload to be present.
 */
bool j2534_hdr_decode(const uint8_t *buf, size_t len, j2534_hdr_t *out);

/**
 * Serialize @p msg to @p buf (cap @p cap). Layout: protocol_id,
 * rx_status, tx_flags, timestamp, extra_data_index, data_size, then
 * data_size bytes. Returns the byte count, or 0 if it doesn't fit / is
 * malformed (data_size > J2534_MAX_DATA).
 */
size_t j2534_msg_encode(const j2534_msg_t *msg, uint8_t *buf, size_t cap);

/**
 * Parse a PASSTHRU_MSG from @p buf/@p len. Returns true on a complete,
 * in-bounds message (data_size <= min(len-24, J2534_MAX_DATA)).
 */
bool j2534_msg_decode(const uint8_t *buf, size_t len, j2534_msg_t *out);

/**
 * J2534 message-filter match: a candidate matches when, over the first
 * @p n bytes (n = min(mask_len, msg_len)), every masked bit is equal:
 *   ((msg[i] ^ pattern[i]) & mask[i]) == 0 for all i in [0, n).
 * A zero-length mask matches everything (PASS-all). Used for PASS/BLOCK
 * filters; FLOW_CONTROL uses the pattern as the ISO-TP address.
 */
bool j2534_filter_match(const uint8_t *mask, size_t mask_len,
                        const uint8_t *pattern, size_t pattern_len,
                        const uint8_t *msg, size_t msg_len);

/** Human name for a status code (static string; "?" if unknown). */
const char *j2534_status_name(uint32_t status);

#ifdef __cplusplus
}
#endif
