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
 * @file autopid_j1939.h
 * @brief The J1939 part of autopid_private.h (TASK_j1939_wwh.md phase 5).
 *        A FRAGMENT: included by autopid_private.h only, after ap_pid_t.
 *
 * A J1939 parameter group is an ordinary PID row whose `cmd` reads
 *
 *   PGN:<hex>[@<source>][?]     PGN:F004      engine controller 1, any source
 *                               PGN:FEEE@0    engine temperature from SA 0
 *                               PGN:FEE5?     engine hours, asked for
 *
 * `<hex>` is the group number (1 to 5 hex digits, a PDU1 group with its
 * destination byte zero), `@<source>` pins a source address (decimal or
 * 0x.., 0..253; without it the listener's pick: the lowest address heard in
 * 5 s), `?` marks a group that is sent on request only: in j1939 `mode`
 * active the row asks for it (j1939_request, to the pinned source or to
 * everyone) at its period and never more than once a second, the answer
 * is read from the store at the next look; a controller's negative
 * acknowledgment fails the row and holds the asking for a minute. In listen
 * mode a `?` row publishes whatever another node asked for. The same
 * parameters and expressions as any row, B0 = the first data byte; `init`
 * and `rxheader` mean nothing here and are refused.
 *
 * Such rows never touch the OBD chip: autopid_runner_j1939.c serves them
 * from the listener's store (the j1939 component), so they keep running
 * while the chip is yielded to an ELM app, held by a diagnostic tool or
 * parked by the bus guard. The scheduler sorts every entry into a class:
 *
 *   chip      a request through the OBD chip (every row until now, filters)
 *   passive   a PGN row: read from the store, nothing transmitted
 *   bus_tx    a PGN row with `?`: a request on the native bus in active
 *             mode; a diagnostics hold stops the asking, not the reading
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

#include "j1939_core.h"     /* J1939_ADDR_ANY (pure)                        */
#include "j1939_spn_core.h" /* the built-in table (pure)                    */

#define AP_PGN_MAX          0x3FFFFu /* 18 bits: EDP, DP, PF, PS            */
#define AP_J1939_SA_MAX     253      /* 254 = null, 255 = global            */
#define AP_PASSIVE_FLOOR_MS 20       /* a PGN row at period 0 (max rate):
                                        looked at every 20 ms, published
                                        when the store has something newer:
                                        no busy spin over the lock          */
#define AP_J1939_STALE_MS   5000     /* a message this old that a row sees
                                        for the first time is not news
                                        (the store's 5 s freshness)         */
#define AP_BUS_TX_FLOOR_MS  1000     /* a `?` row asks at most once a second
                                        (period 0 would be 50 requests/s)  */
#define AP_J1939_NACK_HOLD_MS 60000  /* a group the controller refused is
                                        not asked for again for a minute   */

/* ap_row_class_t and AP_CLASS_* sit next to ap_pid_t in autopid_private.h */

/**
 * Read a `PGN:` command.
 * @param[out] pgn      the group number
 * @param[out] sa       the pinned source, -1 (J1939_ADDR_ANY) when none
 * @param[out] request  the `?` mark
 * @return ESP_OK; ESP_ERR_NOT_FOUND when @p cmd is not a PGN command at
 *         all (a chip request); ESP_ERR_INVALID_ARG when it is one but
 *         malformed (the config is refused). Case-insensitive prefix.
 */
esp_err_t ap_pgn_cmd_parse(const char *cmd, uint32_t *pgn, int16_t *sa,
                           bool *request);

/** `PGN:F004`, `PGN:FEEE@0`, `PGN:FEE5?`: the canonical spelling.
 *  @return its length, 0 when @p cap is too small. */
size_t ap_pgn_cmd_format(uint32_t pgn, int sa, bool request, char *out,
                         size_t cap);

/** The scheduling class of a PID row (filters are AP_CLASS_CHIP). */
ap_row_class_t ap_row_class(const ap_pid_t *pid);

/** "chip", "passive" or "bus_tx". */
const char *ap_row_class_name(ap_row_class_t c);

/**
 * The expression that decodes one SPN of the built-in table over the
 * group's data bytes (B0 = the first), in the grammar the Automate page
 * speaks: J1939 words are little-endian, which the grammar's `[Bx:By]`
 * span is not, so the bytes are composed one at a time:
 *
 *   EngineSpeed (byte 3, 16 bits, 0.125)     (B3+B4*256)*0.125
 *   EngineCoolantTemperature (byte 0, 1)      B0-40
 *   AcceleratorPedalPosition1 (byte 1, 0.4)   B1*0.4
 *   a bit field (byte 4, bit 0, 2 bits)       ((B4>>0)&3)*1
 *
 * @return the length written, 0 when @p cap is too small.
 */
size_t ap_j1939_std_expression(const j1939_spn_t *spn, char *out,
                               size_t cap);

#ifndef AUTOPID_HOST_TEST
/** One SPN of the built-in table as a config row appended to @p arr, in
 *  the shape of ap_std_entry_to_json (`cmd` "PGN:<hex>", `type` "std",
 *  one parameter with the SPN's unit, class and operational range: the
 *  plausibility clamp drops "not available" and error values by itself).
 *  @p sa pins the source (-1 = the pick). (autopid_j1939_std.c) */
bool ap_j1939_std_entry_to_json(cJSON *arr, const j1939_spn_t *spn, int sa);

/** The whole built-in table as config rows (GET /api/autopid/std_table
 *  for a J1939 vehicle): appended to @p arr. @return rows appended. */
size_t ap_j1939_std_table_json(cJSON *arr);

/**
 * The listener's view of the vehicle for the detection job and first
 * contact (autopid_j1939_std.c): is this a J1939 network, who is on it
 * (the source addresses as the responder set, their NAMEs when claimed),
 * the VIN when one was broadcast, and the built-in rows whose group has
 * been seen, appended to @p supported (may be NULL: no rows wanted).
 * @param[out] found  rows appended
 * @return true when the store holds a J1939 bus verdict (the listener
 *         runs and heard well-known groups); @p seen is left alone otherwise.
 */
bool ap_j1939_identify(ap_veh_seen_t *seen, cJSON *supported,
                       uint16_t *found);

/**
 * The same without the listener (can_manager off, or j1939 off): a one
 * second sample of the bus through can_manager_sample_ids (never
 * transmits; a node of its own when the native bus is down). The sources
 * come from the identifiers, no VIN can be read this way, the rows are the
 * groups seen. For the detection job on a device that has not enabled the
 * native bus yet; the scan task's context (it blocks for the sample).
 * @return true when well-known groups were heard.
 */
bool ap_j1939_identify_sample(ap_veh_seen_t *seen, cJSON *supported,
                              uint16_t *found);

/** Is the listener up (enabled, the native bus on)? For the detection
 *  result: a J1939 bus found without it needs can_manager + j1939 enabled
 *  and one restart. */
bool ap_j1939_listening(void);

/* the passive runner (autopid_runner_j1939.c, poller-task context) */
/** Serve a PGN row from the listener's store: publish its parameters when
 *  the store holds a newer message than the last one published for this
 *  row. A `?` row first asks the network for its group when @p tx_ok (the
 *  poller: no diagnostics hold) and the j1939 component holds an address.
 *  @return true when the group is in the store (fresh or not); false =
 *  nobody sent it, or the controller refused the request (the scheduler
 *  backs the row off like a silent PID). */
bool ap_runner_run_j1939(const ap_pid_t *pid, int pid_index,
                         const ap_param_t *params, bool tx_ok);
/** The tables were reloaded: forget what was published. */
void ap_runner_j1939_reset(void);
/** Rows served from the store since boot (published / looked at), for
 *  /api/autopid. */
void ap_runner_j1939_stats(uint32_t *published, uint32_t *looked);
/** Requests `?` rows sent on the bus, and looks refused by a negative
 *  acknowledgment (the row failed), since boot. */
void ap_runner_j1939_tx_stats(uint32_t *requested, uint32_t *refused);
/** Test-a-PID for a PGN command (POST /api/autopid/test, httpd task): the
 *  newest message of @p pgn (from @p sa, -1 = the pick) into @p payload,
 *  "> store / < from source" lines into @p transcript (may be NULL).
 *  ESP_ERR_NOT_FOUND when the store has none. */
esp_err_t ap_runner_test_j1939(uint32_t pgn, int sa, uint8_t *payload,
                               size_t cap, size_t *len, char *transcript,
                               size_t tr_cap);
#endif /* !AUTOPID_HOST_TEST */
