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
 * @file can_selftest.h
 * @brief The CAN self-test (feature component, issue #1023, 2026-10-28):
 *        the ESP32-S3's own CAN controller and the OBD chip share CAN_H and
 *        CAN_L, so one standard OBD exchange between them proves both
 *        transceivers, both directions and the wiring, with or without a
 *        vehicle. The chip asks for engine speed the way every OBD app does
 *        (`010C` to 7DF) and the ESP32 answers it as an engine ECU from 7EF,
 *        ECU number 8's response id, which no production car uses: on a
 *        live vehicle the real ECUs answer too and the test looks only for
 *        its own line.
 *
 * Runs as a job (the HTTP route) or in the caller's task (the console).
 * Nothing persists; the one setting is the console command's presence.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_SELFTEST_STEPS 3
#define CAN_SELFTEST_LINES 12
#define CAN_SELFTEST_LINE  80

/** What the chip printed to the request, classified (pure, host-tested). */
typedef enum
{
    CAN_SELFTEST_REPLY_NONE = 0, /**< nothing, or no prompt in time          */
    CAN_SELFTEST_REPLY_OURS,     /**< the ESP32's line (7EF 04 41 0C ..)     */
    CAN_SELFTEST_REPLY_OTHER,    /**< other ECUs answered, ours never came   */
    CAN_SELFTEST_REPLY_NODATA,   /**< NO DATA: the chip sent, nobody replied  */
    CAN_SELFTEST_REPLY_CANERROR, /**< CAN ERROR / BUS ERROR: no acknowledge   */
    CAN_SELFTEST_REPLY_ERROR,    /**< another error text                     */
} can_selftest_reply_t;

typedef struct
{
    const char *name;
    bool        ok;
    char        detail[112];
    uint32_t    ms;
} can_selftest_step_t;

typedef struct
{
    bool     running;
    bool     done;       /**< a run finished since boot                   */
    bool     pass;
    const char *verdict; /**< one line: PASS or where to look             */
    char     refusal[96];/**< a precondition that stopped the run, or ""  */
    uint32_t baud_kbps;  /**< the bus bitrate the exchange used           */
    bool     live;       /**< a vehicle was talking                       */
    bool     esp_heard;  /**< the ESP32 received the chip's request       */
    can_selftest_reply_t reply;
    int      rpm;        /**< decoded from the chip's line, -1 = none      */
    can_selftest_step_t steps[CAN_SELFTEST_STEPS];
    char     lines[CAN_SELFTEST_LINES][CAN_SELFTEST_LINE]; /* the exchange */
    size_t   n_lines;
    uint32_t took_ms;
    int64_t  at_us;
    esp_err_t err;
} can_selftest_result_t;

/* ---- lifecycle (§3) ---------------------------------------------------------- */

/** Register the settings descriptor ({cli}) and the log descriptor. */
esp_err_t can_selftest_init(void);
/** Nothing runs until a test is asked for: lifecycle parity only. */
esp_err_t can_selftest_start(void);
esp_err_t can_selftest_stop(void);

/* ---- running it ---------------------------------------------------------------- */

/** Run the test on a one-shot task (the HTTP route). ESP_ERR_INVALID_STATE
 *  while one runs; the preconditions are judged by the run itself and land
 *  in the result's refusal. */
esp_err_t can_selftest_run_start(void);

/** Run the test in the caller's task (the console), about 2 s. */
esp_err_t can_selftest_run(can_selftest_result_t *out);

/** The job's result (a copy), running or done. */
void can_selftest_last(can_selftest_result_t *out);

/* ---- pure, host-tested (can_selftest_parse.c) ----------------------------------- */

/** Classify the chip's answer to `010C`: @p resp is what the chip printed
 *  (headers on: `7EF 04 41 0C 1A F8`, one line per response, or an error
 *  text). @p want_id the response id the ESP32 answers from. On OURS,
 *  @p rpm_out gets the decoded engine speed. */
can_selftest_reply_t can_selftest_parse_reply(const char *resp,
                                              uint32_t want_id, int *rpm_out);

/** The one-line verdict for the card, and whether it is a pass. */
const char *can_selftest_verdict(bool esp_heard, can_selftest_reply_t reply,
                                 bool *pass);

/* ---- surfaces ----------------------------------------------------------------- */

/** GET/POST /api/can/selftest (own-routes pattern; main wires it). */
esp_err_t can_selftest_register_http(void);

/** `can_selftest` CLI; registered internally on the settings apply (§6b). */
esp_err_t can_selftest_register_cli(void);

#ifdef __cplusplus
}
#endif
