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
 * @file autopid_resp_private.h
 * @brief What autopid_resp.c (payload assembly) and autopid_resp_lines.c
 *        (the line tokenizer) share. Private to those two files.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AP_LINE_MAX  128
#define AP_MAX_LINES 64

typedef struct
{
    uint8_t bytes[AP_LINE_MAX / 2];
    size_t  n;
    uint32_t header;      /* first token when it isn't a data byte       */
    bool     has_header;
    int      iso_index;   /* "N:" row index, -1 = none                    */
    long     bare_value;  /* whole line was ONE hex token (length line)   */
    bool     is_bare;
    uint8_t  ea;          /* 1 = bytes[0] is an extended address byte (the
                             BMW scheme, headers on): the PCI follows it  */
} ap_line_t;

/** Tokenize raw chip text into parsed data lines (noise and `7F xx 78`
 *  lines dropped, the chip's 29-bit id print taken as a header).
 *  @return line count; error lines set @p saw_error, a bare length line
 *  sets @p iso_total. PURE. */
int ap_resp_collect_lines(const char *resp, ap_line_t *lines, int max,
                          bool *saw_error, long *iso_total);

/** The lines of a reply on a pre-CAN protocol (autopid_resp_precan.c: the
 *  hint of ap_resp_set_proto_hint says it is one) as one payload per ECU,
 *  the lowest address first, laid out as a CAN ECU's answer. @return the
 *  entries written, 0 = not such a reply (no hint, or CAN lines after all):
 *  the caller assembles it the CAN way. Call under the lines lock. PURE.
 *  (ap_resp_ecu_t: autopid_private.h, included ahead of this header.) */
int ap_resp_precan_ecus(const ap_line_t *lines, int n_lines, long iso_total,
                        ap_resp_ecu_t *out, int max_ecus);

#ifdef __cplusplus
}
#endif
