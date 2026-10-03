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
 * @file j1939_claim_core.h
 * @brief SAE J1939-81 network management for a node that wants to talk: its
 *        NAME, the address claim and what to do when somebody else claims
 *        the same address (pure logic, host-testable; time injected).
 *
 * A node may not send anything but an Address Claimed message until it
 * holds an address: it claims one (PGN 60928, destination everyone, data =
 * its NAME) and waits J1939_CLAIM_WAIT_US; nobody contesting means the
 * address is its own. A contest is another node's claim of the same
 * address: the lower NAME (an unsigned 64-bit number, byte 7 the most
 * significant) keeps it and re-sends its claim; the other must move.
 *
 * This device is "arbitrary address capable": it prefers one address
 * (249, off-board diagnostic service tool 1), and on a loss tries 250 (tool
 * 2), then the dynamic range 128..247. Out of addresses it says so once
 * (Cannot Claim Address: the claim message from the null address 254) and
 * stays silent. A Request for Address Claimed is answered with the claim
 * (or the cannot-claim) at any time.
 *
 * The caller supplies the frames: every function that may want one filled
 * fills a j1939_claim_out_t (`send`, source address, 8 data bytes); the
 * frame is PGN 60928 to everyone, priority 6.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "j1939_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define J1939_NAME_LEN        8u
#define J1939_CLAIM_WAIT_US   250000 /**< Hold the address only after this
                                          long without a contest.             */
#define J1939_CLAIM_DEFEND_HOLD_US 250000 /**< One defence per contest round:
                                          a node that keeps claiming our
                                          address after hearing our claim is
                                          broken, and answering every one of
                                          its frames would be a bus storm (the
                                          bench truck's contend mode did 150
                                          rounds in 2 s). The contest is still
                                          counted.                            */
#define J1939_CLAIM_ADDR_MIN  128u   /**< The dynamic range an arbitrary-   */
#define J1939_CLAIM_ADDR_MAX  247u   /**< address node may fall back to.    */
#define J1939_CLAIM_TOOL_1    249u   /**< Off-board diagnostic service tool 1 */
#define J1939_CLAIM_TOOL_2    250u   /**< ... tool 2                          */
#define J1939_FUNCTION_TOOL   129u   /**< NAME function: off-board diagnostic
                                          service tool (industry group 0).    */

typedef enum
{
    J1939_CLAIM_IDLE = 0, /**< Not started: listen mode, or no bus to talk on. */
    J1939_CLAIM_CLAIMING, /**< Claim sent, waiting out J1939_CLAIM_WAIT_US.    */
    J1939_CLAIM_CLAIMED,  /**< The address is ours: the node may transmit.     */
    J1939_CLAIM_CANNOT,   /**< Every address lost: the node stays silent.      */
} j1939_claim_state_t;

typedef struct
{
    uint32_t claims_sent; /**< Address Claimed frames (first, moves, defences,
                               answers to requests).                           */
    uint32_t contests;    /**< Claims of our address by somebody else.         */
    uint32_t won;         /**< Of them: our NAME was lower, we kept it.        */
    uint32_t lost;        /**< Of them: we moved (or gave up).                 */
    uint32_t held;        /**< Of the won: not answered again within
                               J1939_CLAIM_DEFEND_HOLD_US of our last claim.  */
    uint32_t requests;    /**< Requests for Address Claimed answered.          */
    uint32_t cannot;      /**< Cannot Claim Address frames.                    */
} j1939_claim_stats_t;

typedef struct
{
    j1939_claim_state_t state;
    uint8_t  name[J1939_NAME_LEN];
    uint8_t  preferred;   /**< The address asked for first.                   */
    uint8_t  sa;          /**< Ours (CLAIMING / CLAIMED), else J1939_ADDR_NULL. */
    uint8_t  next_dyn;    /**< Next dynamic-range candidate to try.           */
    bool     tried_tool2;
    int64_t  sent_us;     /**< When the current claim went out.               */
    int64_t  defended_us; /**< When we last answered a contest (0: never).    */
    j1939_claim_stats_t stats;
} j1939_claim_t;

/** A frame the caller must send: PGN 60928 to everyone, priority 6. */
typedef struct
{
    bool    send;
    uint8_t sa;                    /**< Ours, or J1939_ADDR_NULL (cannot claim). */
    uint8_t data[J1939_NAME_LEN];  /**< The NAME.                              */
} j1939_claim_out_t;

/**
 * @brief Build a NAME (J1939-81 figure 1, least significant byte first).
 *
 * @param identity      21-bit identity number (unique per device).
 * @param manufacturer  11-bit SAE manufacturer code (0: none assigned).
 * @param function      Function (J1939_FUNCTION_TOOL for this device).
 * @param arbitrary     Arbitrary address capable.
 *
 * ECU instance, function instance, vehicle system (and instance) and the
 * industry group are 0: a global, non-specific system.
 */
void j1939_name_build(uint32_t identity, uint16_t manufacturer,
                      uint8_t function, bool arbitrary,
                      uint8_t out[J1939_NAME_LEN]);

/** Compare two NAMEs as J1939-81 does: negative when @p a has priority
 *  (the lower number), 0 when equal, positive when @p b has it. */
int j1939_name_compare(const uint8_t a[J1939_NAME_LEN],
                       const uint8_t b[J1939_NAME_LEN]);

/** Start over: IDLE with this NAME and preferred address. */
void j1939_claim_init(j1939_claim_t *c, const uint8_t name[J1939_NAME_LEN],
                      uint8_t preferred);

/** Claim the preferred address (-> CLAIMING, @p out = the claim). */
void j1939_claim_start(j1939_claim_t *c, int64_t now_us,
                       j1939_claim_out_t *out);

/** Time passes: CLAIMING becomes CLAIMED after J1939_CLAIM_WAIT_US. */
void j1939_claim_tick(j1939_claim_t *c, int64_t now_us);

/**
 * @brief Somebody's Address Claimed message (@p sa, its NAME) was heard.
 *        Nothing happens unless it is about our address: then the lower
 *        NAME keeps it (@p out = our claim again) or we move to the next
 *        candidate (@p out = the new claim), or give up (@p out = cannot
 *        claim, state CANNOT).
 */
void j1939_claim_rx(j1939_claim_t *c, uint8_t sa,
                    const uint8_t name[J1939_NAME_LEN], int64_t now_us,
                    j1939_claim_out_t *out);

/** A Request for Address Claimed (to us or to everyone): @p out = our
 *  claim, or the cannot-claim; nothing while IDLE. */
void j1939_claim_request(j1939_claim_t *c, int64_t now_us,
                         j1939_claim_out_t *out);

/** True when the node holds an address and may transmit with c->sa. */
bool j1939_claim_ready(const j1939_claim_t *c);

/** "idle", "claiming", "claimed" or "cannot_claim". Never NULL. */
const char *j1939_claim_state_name(j1939_claim_state_t state);

#ifdef __cplusplus
}
#endif
