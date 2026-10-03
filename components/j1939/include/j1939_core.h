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
 * @file j1939_core.h
 * @brief SAE J1939 on the wire: the 29-bit identifier, what kind of frame an
 *        identifier makes, whether a bus speaks J1939 at all, the VIN
 *        message (pure logic, host-testable).
 *
 * The identifier (J1939-21):
 *
 *   28..26  priority
 *   25      extended data page (0 on J1939; 1 = ISO 15765-3, not ours)
 *   24      data page
 *   23..16  PDU format (PF)
 *   15..8   PDU specific (PS): the DESTINATION address when PF < 240
 *           (PDU1, a message sent to someone), part of the group number
 *           when PF >= 240 (PDU2, a broadcast)
 *   7..0    source address
 *
 * A parameter group number (PGN) is data page + PF + PS, with PS zeroed for
 * PDU1. This file never looks at payloads of parameter groups: that is
 * j1939_spn_core.h (values) and j1939_dm_core.h (trouble codes).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define J1939_ADDR_GLOBAL 0xFFu /**< Destination "everyone".                   */
#define J1939_ADDR_NULL   0xFEu /**< Source of a node without an address.      */
#define J1939_ADDR_ANY    (-1)  /**< Lookups: whoever sent it / to whomever.   */

#define J1939_PGN_ACKM    0x00E800u /**< Acknowledgment (ACK / NACK).          */
#define J1939_PGN_REQUEST 0x00EA00u /**< Request for a parameter group.        */
#define J1939_PGN_TP_DT   0x00EB00u /**< Transport protocol, data transfer.    */
#define J1939_PGN_TP_CM   0x00EC00u /**< Transport protocol, connection mgmt.  */
#define J1939_PGN_CLAIM   0x00EE00u /**< Address claimed / cannot claim.       */
#define J1939_PGN_DM1     0x00FECAu /**< Active trouble codes.                 */
#define J1939_PGN_DM2     0x00FECBu /**< Previously active trouble codes.      */
#define J1939_PGN_DM3     0x00FECCu /**< Clear previously active codes (asked).*/
#define J1939_PGN_DM11    0x00FED3u /**< Clear active codes (asked).           */
#define J1939_PGN_VIN     0x00FEECu /**< Vehicle identification.               */

#define J1939_MSG_MAX     1785u /**< Largest message the transport protocol
                                     carries (255 packets of 7 bytes).         */
#define J1939_VIN_LEN     17u

/** A 29-bit identifier taken apart. */
typedef struct
{
    uint8_t  prio;
    uint32_t pgn; /**< PDU1: with the destination byte zeroed.                 */
    uint8_t  sa;
    uint8_t  da;  /**< PDU1: from the identifier; PDU2: J1939_ADDR_GLOBAL.     */
} j1939_id_t;

/** What a received frame is to a J1939 listener. */
typedef enum
{
    J1939_KIND_FOREIGN = 0, /**< Not J1939: an 11-bit or remote frame, or an
                                 identifier with the extended data page bit.   */
    J1939_KIND_DIAG,        /**< ISO 15765 diagnostics on a J1939 bus (groups
                                 DA00 / DB00 and CD00 / CE00): OBD and UDS
                                 requests and answers, somebody else's talk.  */
    J1939_KIND_TP_CM,       /**< Transport protocol: announce, flow, abort.    */
    J1939_KIND_TP_DT,       /**< Transport protocol: a data packet.            */
    J1939_KIND_DATA,        /**< A parameter group in one frame.               */
} j1939_kind_t;

/** True for a PDU1 group: its identifier carries a destination address. */
bool j1939_pgn_is_pdu1(uint32_t pgn);

/** Take a 29-bit identifier apart. */
void j1939_id_parse(uint32_t can_id, j1939_id_t *out);

/** Build a 29-bit identifier. @p da is used by PDU1 groups only. */
uint32_t j1939_id_make(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da);

/**
 * @brief Say what a received frame is. @p out (may be NULL) is filled for
 *        every kind but J1939_KIND_FOREIGN.
 */
j1939_kind_t j1939_classify(uint32_t can_id, bool ext, bool rtr,
                            j1939_id_t *out);

/* ---- does this bus speak J1939? ------------------------------------------- */

typedef enum
{
    J1939_BUS_UNKNOWN = 0, /**< Not enough seen to say.                        */
    J1939_BUS_J1939,       /**< Well-known J1939 groups are broadcast here.    */
    J1939_BUS_OTHER,       /**< Traffic, but none of it J1939.                 */
} j1939_bus_t;

/** Evidence collected frame by frame (zero it to start over). */
typedef struct
{
    uint32_t frames;     /**< Frames looked at.                                */
    uint32_t known;      /**< Of them: a well-known J1939 group, 8 data bytes. */
    uint32_t diag;       /**< ISO 15765 diagnostic identifiers.                */
    uint32_t foreign;    /**< Identifiers a J1939 node cannot send.            */
    uint32_t known_mask; /**< Which well-known groups (one bit each).          */
} j1939_bus_evidence_t;

#define J1939_BUS_MIN_GROUPS 2u  /**< Distinct well-known groups that make a
                                      J1939 bus: one could be a coincidence of
                                      a car's own 29-bit identifiers.          */
#define J1939_BUS_OTHER_AFTER 50u /**< Frames without a single well-known
                                      group: the bus is something else.        */

/** Add one received frame to the evidence. */
void j1939_bus_note(j1939_bus_evidence_t *ev, uint32_t can_id, bool ext,
                    bool rtr, uint8_t dlc);

/** The verdict so far. */
j1939_bus_t j1939_bus_verdict(const j1939_bus_evidence_t *ev);

/** "unknown", "j1939" or "other". Never NULL. */
const char *j1939_bus_name(j1939_bus_t bus);

/* ---- vehicle identification (PGN 65260) ----------------------------------- */

/**
 * @brief The VIN of a Vehicle Identification message.
 *
 * The payload is the VIN in ASCII, closed by '*' (some controllers leave the
 * delimiter out, or pad with spaces, NULs or FF). Only a 17-character VIN of
 * the VIN alphabet (digits and capitals without I, O, Q) is accepted: this
 * string becomes the key of a vehicle.
 *
 * @return true with @p out filled (NUL-terminated); false: no VIN in there.
 */
bool j1939_vin_parse(const uint8_t *data, size_t len,
                     char out[J1939_VIN_LEN + 1]);

/* ---- request and acknowledgment (J1939-21) ---------------------------------- */

#define J1939_REQUEST_LEN 3u  /**< Data bytes of a Request: the group number.  */

#define J1939_ACK_POSITIVE 0u /**< Done (a clear, a command).                  */
#define J1939_ACK_NEGATIVE 1u /**< Not supported (NACK).                       */
#define J1939_ACK_DENIED   2u /**< Access denied (security).                   */
#define J1939_ACK_BUSY     3u /**< Cannot respond now.                         */

/** An Acknowledgment message (PGN 59392) taken apart. */
typedef struct
{
    uint8_t  control;        /**< J1939_ACK_*                                 */
    uint8_t  group_function; /**< 0xFF unless the group has functions.        */
    uint8_t  address;        /**< Whose request is answered (J1939-21 2006+;
                                  0xFF on older controllers).                 */
    uint32_t pgn;            /**< The group the request was for.              */
} j1939_ackm_t;

/** The data of a Request for @p pgn (J1939_REQUEST_LEN bytes). */
void j1939_request_build(uint32_t pgn, uint8_t out[J1939_REQUEST_LEN]);

/** The group a Request asks for. False: not a request (too short). */
bool j1939_request_parse(const uint8_t *d, uint8_t dlc, uint32_t *pgn);

/** The data of an Acknowledgment: @p control for @p address's request of
 *  @p pgn (group function 0xFF). */
void j1939_ackm_build(uint8_t control, uint8_t address, uint32_t pgn,
                      uint8_t out[8]);

/** An Acknowledgment taken apart. False: not one (fewer than 8 bytes). */
bool j1939_ackm_parse(const uint8_t *d, uint8_t dlc, j1939_ackm_t *out);

/** "ack", "nack", "denied", "busy" or "?". Never NULL. */
const char *j1939_ack_name(uint8_t control);

#ifdef __cplusplus
}
#endif
