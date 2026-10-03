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
 * @file j1939.h
 * @brief SAE J1939: what a heavy vehicle's network says, kept as "the newest
 *        message of every parameter group" for whoever wants to read it
 *        (autopid rows, the status page, scripts); and, in active mode, a
 *        node of that network that can ask.
 *
 * The component subscribes to the native CAN bus (can_manager) for every
 * 29-bit frame, puts transport-protocol messages back together and stores
 * the newest payload per (parameter group, source address, destination).
 *
 * In `listen` mode (the default) it NEVER transmits: no address claim, no
 * request, no acknowledge, no flow control. It works on a bus can_manager
 * holds listen-only (`silent`), which is how a vehicle that was not built
 * for this device should be read.
 *
 * In `active` mode (opt-in, 2026-10-03) it claims an address (J1939-81,
 * `address` first, moved on a lost contest), answers a Request for Address
 * Claimed, negatively acknowledges requests addressed to it (it provides no
 * group), receives connections addressed to it (RTS/CTS, with the clear-to-
 * send and end-of-message frames that takes), and lets its readers ask the
 * vehicle: j1939_request() sends a Request for a group (an on-request
 * group, DM2, the VIN, a clear: DM3 / DM11), the answer lands in the store
 * or as an acknowledgment (j1939_request_outcome). It needs can_manager in
 * normal mode (`silent` off); on a listen-only bus it stays a listener and
 * says so.
 *
 * Readers do not get a stream. They ask for a group's newest message
 * (j1939_pgn_latest), one decoded value of the built-in table
 * (j1939_spn_latest), the VIN, the trouble codes of a controller
 * (j1939_dm1, j1939_dm2), and poll j1939_sequence() to know whether anything
 * changed.
 *
 * Settings ("j1939", reboot-to-apply): enabled (default false), mode
 * (listen), address (249), cli. Needs can_manager enabled; with the native
 * bus off j1939_start() succeeds and the component reports that it has
 * nothing to listen to.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "j1939_claim_core.h"
#include "j1939_core.h"
#include "j1939_dm_core.h"
#include "j1939_spn_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    J1939_STATE_OFF = 0,   /**< Disabled by settings (or stopped).             */
    J1939_STATE_NO_BUS,    /**< Enabled, but the native CAN bus is not up.     */
    J1939_STATE_LISTENING, /**< Subscribed: frames are taken in.               */
} j1939_state_t;

/** Counters and state. Every frame taken from the bus is in exactly one of
 *  rx_data, rx_tp_cm, rx_tp_dt, rx_diag, rx_foreign. */
typedef struct
{
    j1939_state_t state;
    j1939_bus_t   bus;         /**< What the traffic looks like so far.        */
    uint32_t rx_frames;        /**< 29-bit frames taken from the bus.          */
    uint32_t rx_data;          /**< Parameter groups in one frame.             */
    uint32_t rx_tp_cm;         /**< Transport protocol: connection management. */
    uint32_t rx_tp_dt;         /**< Transport protocol: data packets.          */
    uint32_t rx_diag;          /**< ISO 15765 frames: not stored.              */
    uint32_t rx_foreign;       /**< Not J1939: not stored.                     */
    uint32_t queue_drops;      /**< Frames lost before this component saw them
                                    (its queue was full): can_manager counts.  */
    uint32_t messages;         /**< Messages stored, frames and long ones.     */
    uint32_t not_kept;         /**< Messages not stored: the store was full.   */
    uint32_t evicted;          /**< Entries dropped for a new key.             */
    uint32_t long_evicted;     /**< Long payloads dropped for a newer one.     */
    uint16_t entries;          /**< Store occupancy.                           */
    uint16_t entries_cap;
    uint16_t sources;          /**< Source addresses heard.                    */
    uint16_t tp_open;          /**< Transport sessions open now.               */
    uint16_t tp_cap;
    uint32_t tp_started;       /**< Transport protocol, see j1939_tp_core.h:   */
    uint32_t tp_completed;     /**< started = completed + seq_errors +         */
    uint32_t tp_seq_errors;    /**< timeouts + aborted + replaced + open.      */
    uint32_t tp_timeouts;
    uint32_t tp_aborted;
    uint32_t tp_replaced;
    uint32_t tp_no_session;
    uint32_t tp_orphan_dt;
    uint32_t tp_bad_cm;
    /* active mode (all zero / false in listen mode) */
    bool     active;           /**< `mode` is active.                          */
    bool     tx_ready;         /**< The bus lets this node transmit now.       */
    j1939_claim_state_t claim; /**< Where the address claim stands.            */
    uint8_t  address;          /**< Ours; J1939_ADDR_NULL while we have none.  */
    uint8_t  name[J1939_NAME_LEN];
    j1939_claim_stats_t claim_stats;
    uint32_t tx_frames;        /**< Frames handed to the bus.                  */
    uint32_t tx_failed;        /**< Frames the bus refused (not ready, full).  */
    uint32_t requests;         /**< Requests sent (j1939_request).             */
    uint32_t acks;             /**< Positive acknowledgments to our requests.  */
    uint32_t nacks;            /**< Negative ones (not supported, denied, busy)*/
    uint32_t nacks_sent;       /**< Requests to us we answered negatively.     */
    uint32_t tp_to_me;         /**< Transport protocol, see j1939_tp_core.h:   */
    uint32_t tp_cts;           /**< connections addressed to us, the clear-to- */
    uint32_t tp_eoma;          /**< send / end-of-message / abort frames we    */
    uint32_t tp_aborts_out;    /**< sent, replies lost to a full queue.        */
    uint32_t tp_reply_lost;
} j1939_status_t;

/** One stored message, as a reader sees it. */
typedef struct
{
    uint32_t pgn;
    uint8_t  sa;
    uint8_t  da;        /**< J1939_ADDR_GLOBAL for a broadcast.                */
    uint16_t len;       /**< Bytes in the message (may exceed the reader's
                             buffer).                                          */
    uint32_t count;     /**< Messages of this key since start.                 */
    uint32_t period_ms; /**< Between the last two; 0 until the second.         */
    uint32_t age_ms;    /**< Since the newest one.                             */
} j1939_msg_t;

/** One source address on the bus. */
typedef struct
{
    uint8_t  sa;
    bool     named;     /**< It claimed its address: name[] is its NAME.       */
    uint8_t  name[8];
    uint32_t frames;
    uint32_t age_ms;    /**< Since its last frame.                             */
} j1939_source_t;

/** "off", "no_bus" or "listening". Never NULL. */
const char *j1939_state_name(j1939_state_t state);

/** Register the settings and log descriptors. Call in main's init pass. */
esp_err_t j1939_init(void);

/** Start listening per settings. ESP_OK when disabled (nothing happens) and
 *  when the native CAN bus is off (J1939_STATE_NO_BUS);
 *  ESP_ERR_INVALID_STATE when the settings could not be applied. */
esp_err_t j1939_start(void);

/** Stop listening. Call before can_manager_stop(). The store keeps what it
 *  has (ages keep growing). */
esp_err_t j1939_stop(void);

/** GET /api/j1939 (HTTP compositions only: main wires it). */
esp_err_t j1939_register_http(void);

esp_err_t j1939_status(j1939_status_t *out);

/** A number that moves whenever a message was stored. A reader that saw it
 *  unchanged knows that reading again gives nothing new. */
uint32_t j1939_sequence(void);

/**
 * @brief The newest message of a parameter group.
 *
 * @param pgn   Group number (PDU1 groups: destination byte zero).
 * @param sa    Source address, or J1939_ADDR_ANY: the lowest address that
 *              sent it within the last 5 s, else the lowest that ever did.
 * @param da    Destination (PDU1 groups), or J1939_ADDR_ANY.
 * @param info  Out (may be NULL).
 * @param buf   Out: the payload, min(len, cap) bytes (may be NULL).
 * @param cap   Room in @p buf.
 * @return ESP_OK; ESP_ERR_NOT_FOUND: nobody sent it (or its long payload had
 *         to make room).
 */
esp_err_t j1939_pgn_latest(uint32_t pgn, int sa, int da, j1939_msg_t *info,
                           uint8_t *buf, size_t cap);

/**
 * @brief One value of the built-in table, from the newest message of its
 *        group.
 *
 * @param spn    Table entry (j1939_spn_find / j1939_spn_table).
 * @param sa     Source address or J1939_ADDR_ANY.
 * @param value  Out: written only when the raw value is valid.
 * @param raw    Out (may be NULL): what the raw value is.
 * @param info   Out (may be NULL): the message it came from.
 * @return ESP_OK when the group was found (check @p raw for the value);
 *         ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t j1939_spn_latest(const j1939_spn_t *spn, int sa, double *value,
                           j1939_raw_t *raw, j1939_msg_t *info);

/** The VIN somebody sent (PGN 65260). False: none seen yet. */
bool j1939_vin(char out[J1939_VIN_LEN + 1], uint8_t *sa);

/**
 * @brief Active trouble codes (DM1) as controller @p sa last broadcast them.
 *
 * @param lamps  Out (may be NULL).
 * @param out    Out: the codes (may be NULL with @p max 0).
 * @param count  Out: codes in the message, which may exceed @p max.
 * @param info   Out (may be NULL): the message.
 * @return ESP_OK; ESP_ERR_NOT_FOUND: that controller sent no DM1.
 */
esp_err_t j1939_dm1(uint8_t sa, j1939_lamps_t *lamps, j1939_dtc_t *out,
                    size_t max, size_t *count, j1939_msg_t *info);

/** Previously active trouble codes (DM2) as controller @p sa last sent
 *  them: a group that is only ever answered to a request (active mode,
 *  j1939_request(J1939_PGN_DM2, ...)). Arguments as j1939_dm1. */
esp_err_t j1939_dm2(uint8_t sa, j1939_lamps_t *lamps, j1939_dtc_t *out,
                    size_t max, size_t *count, j1939_msg_t *info);

/* ---- active mode ------------------------------------------------------------ */

/** True when the node holds an address: requests go out. False in listen
 *  mode, while the claim is pending, when every address was lost, and when
 *  the bus is listen-only. */
bool j1939_active(void);

/** The node's address, or J1939_ADDR_NULL. */
uint8_t j1939_address(void);

typedef enum
{
    J1939_REQ_NONE = 0, /**< Never asked (or the slot was reused).             */
    J1939_REQ_PENDING,  /**< Asked; no acknowledgment heard (the data, if any,
                             is in the store).                                 */
    J1939_REQ_ACKED,    /**< Positively acknowledged (a clear was done).       */
    J1939_REQ_NACKED,   /**< Negatively acknowledged: not supported, access
                             denied or busy (`control` says which).            */
} j1939_req_outcome_t;

/**
 * @brief Ask the network for a parameter group (PGN 59904 from our address,
 *        priority 6). The answer arrives later: a group's data in the store
 *        (poll j1939_pgn_latest / j1939_sequence), or an acknowledgment
 *        (j1939_request_outcome). One slot per (group, destination): asking
 *        again replaces the outcome.
 *
 * @param pgn  The group (DM2, the VIN, an on-request group, DM3 / DM11 to
 *             clear codes).
 * @param da   One controller, or J1939_ADDR_GLOBAL (everyone answers;
 *             nobody acknowledges negatively).
 * @return ESP_OK: sent. ESP_ERR_INVALID_STATE: listen mode, no address
 *         yet / at all, or the bus is listen-only. ESP_FAIL: the bus did not
 *         take the frame.
 */
esp_err_t j1939_request(uint32_t pgn, uint8_t da);

/** What became of the last request for (@p pgn, @p da). @p age_ms (may be
 *  NULL): since it was sent; @p control (may be NULL): the acknowledgment's
 *  control byte (J1939_ACK_*). */
j1939_req_outcome_t j1939_request_outcome(uint32_t pgn, uint8_t da,
                                          uint32_t *age_ms, uint8_t *control);

/** "none", "pending", "acked" or "nacked". Never NULL. */
const char *j1939_req_outcome_name(j1939_req_outcome_t outcome);

/**
 * @brief Walk the stored messages. Start with *cursor = 0; each call fills
 *        @p info and @p buf (as j1939_pgn_latest) and advances the cursor.
 * @return false at the end. The store may change between calls: a walk is a
 *         view, not a snapshot.
 */
bool j1939_msg_next(size_t *cursor, j1939_msg_t *info, uint8_t *buf,
                    size_t cap);

/** The source addresses heard, lowest first. @return How many were written. */
size_t j1939_sources(j1939_source_t *out, size_t max);

/** One source address. False: nothing was heard from it. */
bool j1939_source(uint8_t sa, j1939_source_t *out);

/** Zero the counters and forget every stored message (console `j1939 -z`). */
void j1939_reset(void);

/** Occupancy of the message store and of the transport sessions (boot
 *  health report). */
void j1939_capacity(size_t *used, size_t *cap);
void j1939_tp_capacity(size_t *used, size_t *cap);

#ifdef __cplusplus
}
#endif
