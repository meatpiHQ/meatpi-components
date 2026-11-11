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
 * @file autopid_reply_private.h
 * @brief The reply part of autopid_private.h: the chip's response text to
 *        bytes, the monitor stream collector and the echo check
 *        (autopid_resp.c and its neighbours, pure). What those two files
 *        share between themselves is autopid_resp_private.h.
 *        A FRAGMENT: included by autopid_private.h only (it needs its
 *        base types); split out 2026-11-11 to keep autopid_private.h
 *        under the 700-line rule.
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

/* ---- pure: ELM response text -> bytes (autopid_resp.c) ---------------------
 * Handles: echo/blank/SEARCHING lines, error markers (NO DATA, ERROR,
 * STOPPED, ?), headers-off single line ("41 0C 1A F8"), headers-off
 * ISO-TP multi-line ("014" + "0: 49 02 .." + "1: .."), headers-on frames
 * ("7E8 06 41 00 ..") incl. ISO-TP single/first/consecutive reassembly
 * from the lowest-ID responder, an extended address byte before the PCI
 * ("607 F1 10 09 62 ..", the BMW scheme), the chip's four-token 29-bit id.
 * The PAYLOAD is framing-free, from the service echo (B0 = 0x41): what the
 * parsers (VIN, DTC, the detection) and the cross-talk guard read.      */
esp_err_t ap_resp_to_payload(const char *resp, uint8_t *payload,
                             size_t payload_max, size_t *out_len);

/** The reply as the chip prints it with headers on, which is what user
 *  expressions index since 2026-10-24 (issue #1014; the legacy firmware's
 *  parse_elm327_response() read the same bytes, so the published vehicle
 *  profiles count this way): every line's bytes after the CAN id, the
 *  ISO-TP PCI bytes and an extended address byte included, the lines of
 *  the LOWEST responder id concatenated in arrival order, `7F xx 78`
 *  response-pending frames dropped. The chip trims a single frame to its
 *  length and prints consecutive frames whole, padding included. A reply
 *  that arrives without ids (a K-line protocol, whose prelude keeps
 *  headers off) is rebuilt into that shape from its rows: `0L` + the
 *  bytes, or `1L LL` + six bytes then `2N` + seven per consecutive frame,
 *  so `[B3:B4]/4` is the RPM on every protocol. PURE.
 *  @return ESP_ERR_NOT_FOUND = no data line, ESP_FAIL = an error line. */
esp_err_t ap_resp_frames(const char *resp, uint8_t *out, size_t out_max,
                         size_t *out_len);

/** How the tokenizer reads a 29-bit id printed as four byte tokens: the
 *  legislated `18 DA F1 xx` is always taken (take_id29); with this hint a
 *  row's own receive filter id (`ATCRA17FE007B`, the MEB profile) is taken
 *  too, and on a 29-bit protocol any four tokens followed by a plausible
 *  PCI. Set by the runner before a row's reply is parsed (the poller task)
 *  and by the test route (the httpd task): never at once (the chip job).
 *  0 / false = the legislated rule alone. */
void ap_resp_set_id29_hint(uint32_t rx_id, bool proto29);

#define AP_RESP_ECUS_MAX 8      /* 7E8..7EF: the functional-response set */

typedef struct
{
    uint32_t header;            /* CAN id; UINT32_MAX = headers were off */
    uint8_t  payload[AP_PAYLOAD_MAX];
    size_t   len;
} ap_resp_ecu_t;

/** Like ap_resp_to_payload but keeps EVERY responder when headers are
 *  on: one entry per distinct CAN id (ascending), each ISO-TP
 *  reassembled independently. Headers off = single entry (identical to
 *  ap_resp_to_payload). @return entry count, 0 = no payload,
 *  -1 = chip/bus error line seen. */
int ap_resp_to_payloads(const char *resp, ap_resp_ecu_t *out,
                        int max_ecus);

/** One ATMA monitor line -> frame data bytes when it carries @p frame_id
 *  (legacy-compatible header shapes: contiguous "7E8"/"18DAF110" or the
 *  id split into 2-hex byte tokens). Expressions index from B0 = first
 *  DATA byte (no id echo: the legacy filter frame-of-reference). PURE. */
bool ap_filter_frame(const char *line, size_t len, uint32_t frame_id,
                     uint8_t *payload, size_t payload_max,
                     size_t *out_len);

/** Incremental ATMA stream collector: feed raw monitor bytes in ANY
 *  chunking; returns true the moment a completed line carries
 *  @p frame_id (first match wins: repeats of the id in the same
 *  window are simply never reached). Overlong lines are truncated
 *  safely; non-matching/noise lines are skipped. PURE. */
typedef struct
{
    char   line[160];
    size_t n;
    bool   overflow;            /* current line exceeded the buffer      */
} ap_flt_stream_t;

void ap_flt_stream_init(ap_flt_stream_t *st);
bool ap_flt_stream_feed(ap_flt_stream_t *st, const uint8_t *bytes,
                        size_t len, uint32_t frame_id, uint8_t *payload,
                        size_t payload_max, size_t *out_len);

/** ap_flt_stream_feed that reports how many input bytes were consumed
 *  when a frame matched (up to and including its line terminator), so
 *  the caller can resume mid-chunk and capture EVERY matching frame:
 *  a multiplexed message needs more than the first one. On false the
 *  whole chunk was consumed. */
bool ap_flt_stream_feed_ex(ap_flt_stream_t *st, const uint8_t *bytes,
                           size_t len, uint32_t frame_id,
                           uint8_t *payload, size_t payload_max,
                           size_t *out_len, size_t *consumed);

/** True when @p payload plausibly answers @p cmd: hex service commands
 *  must echo (service | 0x40) + the identifier byte. Rejects cross-talk
 *  when another chip master's response lands in our request window
 *  (bench-proven under WS-OBD contention, Phase 1b). Service 22 echoes
 *  both identifier bytes, and both are compared: `62 F4 0D` does not
 *  answer `22F40C` (an ECU's late answer to the previous request, bench
 *  2026-10-03). AT/ST/VT commands return true (nothing to verify). PURE. */
bool ap_payload_matches_cmd(const char *cmd, const uint8_t *payload,
                            size_t payload_len);
