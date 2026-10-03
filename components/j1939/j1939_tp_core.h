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
 * @file j1939_tp_core.h
 * @brief J1939-21 transport protocol, the LISTENING side: messages of 9 to
 *        1785 bytes put back together from what passes on the bus (pure
 *        logic, host-testable; the caller supplies the time).
 *
 * Two ways a long message travels:
 *
 *   BAM       announced to everyone (TP.CM control 32, destination FF), then
 *             the data packets 50 to 200 ms apart. Nobody answers.
 *   RTS/CTS   a connection between two nodes: request to send (16), clear to
 *             send (17) from the receiver, packets, end of message
 *             acknowledge (19). Either side may abort (255).
 *
 * A listener follows both from the outside: it sends nothing, so a
 * connection between two other nodes is read as it goes by (their CTS tells
 * which packet comes next). One session per (sender, destination) pair, as
 * the standard allows; J1939_TP_SESSIONS of them at a time.
 *
 * With an address of its own (`my_sa`, active mode) the node is also a
 * DESTINATION: a request-to-send addressed to it is answered with a clear-
 * to-send, the completed message with an end-of-message acknowledge, and a
 * session that goes wrong with a connection abort. Those frames are queued
 * as replies (j1939_tp_reply_take): the caller sends them, this module
 * never does. Without an address (my_sa = J1939_ADDR_NULL) nothing is
 * queued and the node is the listener above.
 *
 * Nothing fails silently: every announce is counted (started, no_session or
 * bad_cm), every started session ends as exactly one of completed,
 * seq_errors, timeouts, aborted or replaced, and a data packet that belongs
 * to no session is an orphan_dt.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "j1939_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define J1939_TP_SESSIONS   8u
#define J1939_TP_TIMEOUT_US 1250000 /**< No frame of a session for this long:
                                         it is over (the standard's T1 is
                                         750 ms, T2 / T3 are 1250 ms).         */

#define J1939_TP_CTRL_RTS   16u
#define J1939_TP_CTRL_CTS   17u
#define J1939_TP_CTRL_EOMA  19u
#define J1939_TP_CTRL_BAM   32u
#define J1939_TP_CTRL_ABORT 255u

/* connection abort reasons (J1939-21 table 7) this node gives */
#define J1939_TP_ABORT_BUSY     1u /**< No session free for another connection. */
#define J1939_TP_ABORT_TIMEOUT  3u /**< The sender went quiet.                  */
#define J1939_TP_ABORT_SEQUENCE 7u /**< A packet out of sequence.               */

#define J1939_TP_REPLIES    8u /**< Replies queued before the caller takes them. */

typedef struct
{
    uint32_t started;    /**< Announces accepted (BAM or RTS).                 */
    uint32_t completed;  /**< Messages handed out.                             */
    uint32_t seq_errors; /**< Sessions ended by a packet out of sequence, or
                              closed by the receiver's acknowledge while
                              packets were still missing here.                 */
    uint32_t timeouts;   /**< Sessions that went quiet.                        */
    uint32_t aborted;    /**< Sessions ended by a connection abort.            */
    uint32_t replaced;   /**< Sessions ended by a new announce from the same
                              sender to the same destination.                  */
    uint32_t no_session; /**< Announces ignored: every session was busy.       */
    uint32_t orphan_dt;  /**< Data packets that belong to no open session.     */
    uint32_t bad_cm;     /**< Connection management frames that cannot be
                              (length, size, packet count, unknown control).   */
    uint32_t to_me;      /**< Requests to send addressed to this node.         */
    uint32_t cts;        /**< Clear-to-send replies queued.                    */
    uint32_t eoma;       /**< End-of-message acknowledges queued.              */
    uint32_t aborts_out; /**< Connection aborts queued.                        */
    uint32_t reply_lost; /**< Replies not queued: the queue was full.          */
} j1939_tp_stats_t;

typedef struct
{
    bool     open;
    bool     mine;    /**< Addressed to this node: it answers.                 */
    uint8_t  sa;      /**< The sender of the message.                          */
    uint8_t  da;      /**< J1939_ADDR_GLOBAL for a BAM.                        */
    uint8_t  packets; /**< Announced.                                          */
    uint8_t  next;    /**< Sequence number expected next, from 1.              */
    uint8_t  cts_max; /**< Packets the sender takes per clear-to-send.        */
    uint8_t  cts_left;/**< Packets still due in the current window.           */
    uint16_t size;    /**< Announced bytes.                                    */
    uint32_t pgn;     /**< The group the message carries.                      */
    int64_t  last_us; /**< Its last frame.                                     */
} j1939_tp_session_t;

/** A TP.CM frame this node must send: from my_sa to @p da, priority 7. */
typedef struct
{
    uint8_t da;
    uint8_t data[8];
} j1939_tp_reply_t;

/** The whole receiver. About 14.5 KB: keep it static, in PSRAM. */
typedef struct
{
    j1939_tp_session_t s[J1939_TP_SESSIONS];
    uint8_t            buf[J1939_TP_SESSIONS][J1939_MSG_MAX];
    j1939_tp_stats_t   stats;
    uint8_t            my_sa;   /**< This node's address; J1939_ADDR_NULL = none. */
    j1939_tp_reply_t   reply[J1939_TP_REPLIES];
    uint8_t            reply_n;
} j1939_tp_t;

/** A message put back together. */
typedef struct
{
    uint32_t       pgn;
    uint8_t        sa;
    uint8_t        da;
    uint16_t       len;
    const uint8_t *data; /**< Into the receiver: valid until the next call
                              with the same j1939_tp_t.                        */
} j1939_tp_msg_t;

void j1939_tp_init(j1939_tp_t *tp);

/** A TP.CM frame sent by @p sa to @p da. */
void j1939_tp_cm(j1939_tp_t *tp, uint8_t sa, uint8_t da, const uint8_t *d,
                 uint8_t dlc, int64_t now_us);

/**
 * @brief A TP.DT frame sent by @p sa to @p da.
 * @return true when it completed a message: @p out describes it.
 */
bool j1939_tp_dt(j1939_tp_t *tp, uint8_t sa, uint8_t da, const uint8_t *d,
                 uint8_t dlc, int64_t now_us, j1939_tp_msg_t *out);

/** Close the sessions that went quiet. Call a few times a second. */
void j1939_tp_expire(j1939_tp_t *tp, int64_t now_us);

/** Sessions open right now. */
size_t j1939_tp_open(const j1939_tp_t *tp);

/** This node's address: requests to send addressed to it are answered from
 *  now on. J1939_ADDR_NULL (the init value): a listener only. Sessions
 *  addressed to a previous address run out on their own. */
void j1939_tp_set_address(j1939_tp_t *tp, uint8_t my_sa);

/** Take the next reply this node must send (false: none queued). Call
 *  after every j1939_tp_cm / j1939_tp_dt / j1939_tp_expire until false. */
bool j1939_tp_reply_take(j1939_tp_t *tp, j1939_tp_reply_t *out);

#ifdef __cplusplus
}
#endif
