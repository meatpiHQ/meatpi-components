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
 * @file usb_acm_nmea.h
 * @brief NMEA 0183 from a plain USB GNSS receiver (a u-blox 7 on the bench:
 *        CDC-ACM, RMC/GGA/GSA/GSV/GLL/VTG at 1 Hz, unprompted) into the same
 *        fix struct the ESPNetLink's console JSON fills. Pure, no deps,
 *        host-tested: bytes in (any chunking), sentences out, a fix out of
 *        every RMC with status A, merged with the last GGA's satellites,
 *        HDOP and altitude. 2026-10-07, Quick Setup's USB step.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb_acm_gps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_ACM_NMEA_LINE_MAX 100   /* NMEA 0183 caps a sentence at 82 */

typedef struct
{
    /* the line being collected from the byte stream */
    char     line[USB_ACM_NMEA_LINE_MAX + 1];
    size_t   line_len;
    bool     overflow;      /**< the line ran past the cap: dropped at its end */
    /* what RMC does not carry, from the last GGA */
    bool     have_gga;
    int      quality;       /**< GGA fix quality, 0 = none                     */
    int      satellites;
    double   hdop;
    double   altitude_m;
    /* counters for the status surfaces */
    uint32_t sentences;     /**< well-formed sentences seen (checksum ok)      */
    uint32_t bad_checksum;
} usb_acm_nmea_t;

void usb_acm_nmea_init(usb_acm_nmea_t *st);

/** True for a well-formed GNSS sentence: `$` + a two-letter talker + a
 *  three-letter formatter, `,`, and a `*hh` checksum that matches. */
bool usb_acm_nmea_is_sentence(const char *line);

/**
 * Feed one sentence (no CR/LF). Returns 1 and fills @p out when the line is
 * an RMC with status A (a live fix; `satellites`, `altitude_m` and
 * `accuracy_m` come from the last GGA, 0 without one), -1 for an RMC with
 * status V (the receiver lost its fix: the caller invalidates its cache),
 * 0 for anything else (GGA is remembered, other sentences are counted).
 * Checksum failures count and yield 0.
 */
int usb_acm_nmea_feed_line(usb_acm_nmea_t *st, const char *line,
                           usb_acm_gps_t *out);

/**
 * Feed raw bytes as they arrive (any chunking: a sentence may span calls).
 * Returns 1 with @p out = the last fix produced by the chunk, -1 when the
 * chunk ended on a lost fix (an RMC V with no fix after it), 0 otherwise.
 */
int usb_acm_nmea_feed_bytes(usb_acm_nmea_t *st, const uint8_t *data,
                            size_t len, usb_acm_gps_t *out);

#ifdef __cplusplus
}
#endif
