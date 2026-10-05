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
 * @file uds_dtc.h
 * @brief PURE UDS DTC codec (ISO 14229 services 0x19 / 0x14):
 *        request builders + response parsers, host-tested. The
 *        consumer (autopid_dtc, TASK_dtc §12) drives the transactions
 *        via uds_request(); this layer never does I/O.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ISO 14229 DTC status bits (the two the scan uses) */
#define UDS_DTC_STATUS_PENDING   0x04u /* pendingDTC                    */
#define UDS_DTC_STATUS_CONFIRMED 0x08u /* confirmedDTC                  */

/** One decoded DTC record from a 0x19 response. */
typedef struct
{
    uint8_t hi;     /**< DTCHighByte  (letter + first digit encoding)   */
    uint8_t mid;    /**< DTCMiddleByte                                  */
    uint8_t ftb;    /**< DTCLowByte = failure type byte (sub-type)      */
    uint8_t status; /**< status byte                                    */
} uds_dtc_t;

/* ---- request builders (return bytes written) ------------------------------- */

/** 19 01 <mask>: reportNumberOfDTCByStatusMask. */
size_t uds_dtc_req_count(uint8_t status_mask, uint8_t out[3]);

/** 19 02 <mask>: reportDTCByStatusMask. */
size_t uds_dtc_req_by_status(uint8_t status_mask, uint8_t out[3]);

/** 19 0A: reportSupportedDTC. */
size_t uds_dtc_req_supported(uint8_t out[2]);

/** 14 <g><g><g>: ClearDiagnosticInformation. NULL group = FFFFFF
 *  (all groups). */
size_t uds_dtc_req_clear(const uint8_t group[3], uint8_t out[4]);

/* ---- response parsers ------------------------------------------------------- */

/** Parse `59 01 <availMask> <fmt> <count16>`. @return true on shape
 *  match. */
bool uds_dtc_parse_count(const uint8_t *resp, size_t len,
                         uint8_t *avail_mask, uint16_t *count);

/** Parse `59 02|0A <availMask> (Hi Mid Low Status)×N`. Truncated
 *  trailing records are ignored (keep what fits @p max too).
 *  @return record count, or -1 when the header doesn't match. */
int uds_dtc_parse_list(const uint8_t *resp, size_t len,
                       uint8_t *avail_mask, uds_dtc_t *out, size_t max);

/** `54` positive ClearDiagnosticInformation response. */
bool uds_dtc_clear_ok(const uint8_t *resp, size_t len);

/* ---- code text -------------------------------------------------------------- */

/** hi/mid -> "P0420" and, when @p ftb != 0, append "-08" (2-hex FTB).
 *  FTB 0 omits the suffix so obd- and uds-sourced codes for the same
 *  fault are IDENTICAL strings (stable new-code diffs across `auto`).
 *  @p out must hold >= 10 bytes. */
void uds_dtc_format(uint8_t hi, uint8_t mid, uint8_t ftb, char *out);

/** "P0420" / "P0420-08" -> triple. @return false on malformed input. */
bool uds_dtc_unformat(const char *code, uint8_t *hi, uint8_t *mid,
                      uint8_t *ftb);

/* ---- WWH-OBD (ISO 27145-3) and SAE J1979-2 ---------------------------------
 * The legislated trouble codes of a vehicle that speaks OBD over UDS:
 * service 19 sub-function 42 (by status and severity mask) and 55
 * (permanent status), both for a functional group; the emissions group
 * 0x33 is the one OBD means. The answer names its DTC format:
 * 0x04 = SAE J2012-DA format 04 (the P/C/B/U code + a failure type byte,
 * "P0420" / "P2463-1F"), 0x02 = SAE J1939-73 (SPN + FMI, "SPN3226-4").   */

#define UDS_WWH_FGID_EMISSIONS  0x33u
/** The severity mask the legislated testers send: the four WWH-OBD DTC
 *  classes A, B1, B2 and C (bits 1..4). */
#define UDS_WWH_SEVERITY_CLASSES 0x1Eu
#define UDS_DTC_FORMAT_J1939    0x02u
#define UDS_DTC_FORMAT_J2012_04 0x04u
#define UDS_DTC_TEXT_LEN        16     /* "SPN524287-31" + NUL, with room */

/** One record of a 59 42 / 59 55 answer. */
typedef struct
{
    uint8_t dtc[3];     /**< the three DTC bytes, as sent                */
    uint8_t status;     /**< statusOfDTC                                 */
    uint8_t severity;   /**< severity + class byte (0 in a 59 55 answer) */
} uds_wwh_dtc_t;

/** 19 42 <group> <status mask> <severity mask>. */
size_t uds_wwh_req_by_mask(uint8_t group, uint8_t status_mask,
                           uint8_t severity_mask, uint8_t out[5]);

/** 19 55 <group>: the DTCs with permanent status. */
size_t uds_wwh_req_permanent(uint8_t group, uint8_t out[3]);

/** 14 FF FF <group>: clear the group (ISO 27145-3: all of it or
 *  nothing; there is no clear of a single code). */
size_t uds_wwh_req_clear(uint8_t group, uint8_t out[4]);

/** Parse `59 42 <group> <statusAvail> <severityAvail> <format>
 *  (severity dtc dtc dtc status)xN`. A truncated last record is ignored,
 *  records beyond @p max are dropped.
 *  @return record count, -1 when it is not such an answer for @p group. */
int uds_wwh_parse_by_mask(const uint8_t *resp, size_t len, uint8_t group,
                          uint8_t *format, uds_wwh_dtc_t *out, size_t max);

/** Parse `59 55 <group> <statusAvail> <format> (dtc dtc dtc status)xN`. */
int uds_wwh_parse_permanent(const uint8_t *resp, size_t len, uint8_t group,
                            uint8_t *format, uds_wwh_dtc_t *out, size_t max);

/** The code as text: format 0x02 = "SPN<spn>-<fmi>" (J1939-73: 19 bits of
 *  SPN, 5 of FMI), every other format the J2012 text of uds_dtc_format()
 *  ("P0420", "P2463-1F"). */
void uds_wwh_dtc_text(uint8_t format, const uds_wwh_dtc_t *dtc,
                      char out[UDS_DTC_TEXT_LEN]);

#ifdef __cplusplus
}
#endif
