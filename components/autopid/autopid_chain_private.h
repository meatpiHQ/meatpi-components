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
 * @file autopid_chain_private.h
 * @brief The chain part of autopid_private.h: what a chain of the tables (a
 *        type's or a row's init, a cmd) sets on the chip, what puts the
 *        baseline back and how its replies read (autopid_chain.c, pure).
 *        A FRAGMENT: included by autopid_private.h only (it needs its
 *        base types); split out 2026-11-11 to keep autopid_private.h
 *        under the 700-line rule.
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

/* ---- pure: what a chain of the tables sets on the chip (autopid_chain.c,
 * host-tested). Issue #964: a custom row's init moved the chip to a BMW
 * gateway (ATSH6F1;ATCEA60;ATCRA660;ATFCSM1...) and the standard rows that
 * followed went there too. The runner restores the baseline before the next
 * row type when a chain moved any of this. ---------------------------------- */
#define AP_SETS_HEADER    (1u << 0)  /* ATSH: the prelude's functional header */
#define AP_SETS_FILTER    (1u << 1)  /* ATCRA / ATCM / ATCF: the prelude's ATCRA */
#define AP_SETS_PROTOCOL  (1u << 2)  /* ATSP / ATTP: the prelude's ATTP<p>  */
#define AP_SETS_PRINT     (1u << 3)  /* ATH0 / ATS0 / ATST: ATH1 ATS1 ATST96 */
#define AP_SETS_EXT_ADDR  (1u << 4)  /* ATCEA / ATCER -> ATCEA              */
#define AP_SETS_FLOW_CTL  (1u << 5)  /* ATFCSH / ATFCSD / ATFCSM -> ATFCSM0 */
#define AP_SETS_FC_OFF    (1u << 6)  /* ATCFC0 -> ATCFC1                    */
#define AP_SETS_TIMING    (1u << 7)  /* ATAT0 / ATAT2 -> ATAT1              */
#define AP_SETS_RAW       (1u << 8)  /* ATCAF0 -> ATCAF1                    */
#define AP_SETS_PRIORITY  (1u << 9)  /* ATCP -> ATCP18                      */
#define AP_SETS_ECHO      (1u << 10) /* ATE1 -> ATE0                        */
#define AP_SETS_LINEFEED  (1u << 11) /* ATL1 -> ATL0                        */
#define AP_SETS_QUIET     (1u << 12) /* ATR0 -> ATR1                        */
#define AP_SETS_DLC       (1u << 13) /* ATV1 / ATD1 -> ATV0 ATD0            */

/** The AP_SETS_* bits a ';'-separated chain (a type's or a row's init, an
 *  AT command as a row's cmd) sets on the chip beyond the baseline. Read as
 *  the chip reads it: spaces ignored, any case; a command the table does not
 *  know sets nothing; a bare `ATCEA` / `ATCRA` (the off / reset forms) set
 *  nothing. NULL or no AT command = 0. */
unsigned ap_chain_sets(const char *chain);

/** The chain that puts the baseline back for @p sets: @p prelude (which
 *  covers header, filter, protocol and print format) followed by one undo
 *  command per remaining bit (`;ATCEA;ATFCSM0;...`). @return the length
 *  written (cap - 1 when it did not fit: AP_RESTORE_LEN always does). */
size_t ap_chain_restore(unsigned sets, const char *prelude, char *out,
                        size_t cap);

/** Drop from a chain of the tables the print-format commands the poller
 *  cannot live with, as the legacy firmware did (its send_commands skipped
 *  `ATH0`, `ATS0` and `ATE1`): autopid reads the chip with headers and
 *  spaces on and the echo off, and a published profile's `ATS0` (the Mini
 *  Cooper SE) or `ATH0` (the Ford profiles) would break every reply after
 *  it. In place; spaces and case ignored; the separators tidied.
 *  @return how many commands were dropped. PURE. */
int ap_chain_strip_print(char *chain);

/** The receive filter a chain sets (`ATCRA<id>`, the last one wins): the
 *  id and whether it is 29-bit (8 hex digits). @return false when the chain
 *  sets none, or resets it (a bare `ATCRA`) last. PURE. */
bool ap_chain_cra_id(const char *chain, uint32_t *id, bool *is_29bit);

/** What a chain of the tables changes about how its replies read
 *  (ap_resp_set_id29_hint), element by element as the chip takes them:
 *  @p rx_id = the 29-bit id its `ATCRA` names (0 after an 11-bit or a bare
 *  one), @p pin = the protocol its `ATSP` / `ATTP` sets. Both stay as they
 *  are when the chain says nothing about them, so a caller carries them
 *  from one chain to the next as the chip carries the settings. PURE. */
void ap_chain_reply_hint(const char *chain, uint32_t *rx_id, char *pin);
