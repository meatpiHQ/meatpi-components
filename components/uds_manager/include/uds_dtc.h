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
 * @brief PURE UDS DTC codec (ISO 14229 services 0x19 / 0x14) —
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

/** 19 01 <mask> — reportNumberOfDTCByStatusMask. */
size_t uds_dtc_req_count(uint8_t status_mask, uint8_t out[3]);

/** 19 02 <mask> — reportDTCByStatusMask. */
size_t uds_dtc_req_by_status(uint8_t status_mask, uint8_t out[3]);

/** 19 0A — reportSupportedDTC. */
size_t uds_dtc_req_supported(uint8_t out[2]);

/** 14 <g><g><g> — ClearDiagnosticInformation. NULL group = FFFFFF
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

#ifdef __cplusplus
}
#endif
