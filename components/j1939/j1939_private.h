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
 * @file j1939_private.h
 * @brief What the files of the j1939 component share: the settings getters,
 *        the console registration, the listener's state behind its lock and
 *        the transmitting side of active mode (j1939_tx.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "j1939.h"
#include "j1939_cache_core.h"
#include "j1939_claim_core.h"
#include "j1939_tp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- settings (j1939_settings.c) ---------------------------------------------- */

esp_err_t j1939_settings_register(void);

/** False when the boot apply failed even with defaults (Standard 4.3 step 5). */
bool j1939_settings_is_configured(void);

bool j1939_settings_enabled(void);

/** `mode` = active: the node claims an address and talks. */
bool j1939_settings_active(void);

/** `address`: the one claimed first (J1939_CLAIM_TOOL_1 by default). */
uint8_t j1939_settings_address(void);

/* ---- console (j1939_cli.c) ------------------------------------------------------ */

/** The `j1939` command. Called from the settings apply, behind `cli`. */
esp_err_t j1939_register_cli(void);

/* ---- the listener's state (j1939.c) ---------------------------------------------- */

/** One lock for the store, the transport sessions and the sources. The
 *  receive task holds it for a batch of frames (microseconds); a reader
 *  holds it for a lookup and a copy, never across anything that can block. */
void j1939_lock(void);
void j1939_unlock(void);

/** The store, the sources and the transport sessions: only between
 *  j1939_lock() and j1939_unlock(). */
j1939_cache_t   *j1939_priv_cache(void);
j1939_sources_t *j1939_priv_sources(void);
j1939_tp_t      *j1939_priv_tp(void);

/* ---- active mode: the transmitting side (j1939_tx.c) ------------------------------- */

/** Prepare for a session on the bus (j1939_start): the NAME, the claim
 *  state machine, the counters of the session. Nothing is sent yet. */
void j1939_tx_start(void);

/** The session is over (j1939_stop): back to a listener, address dropped. */
void j1939_tx_stop(void);

/** Time passes (receive-task loop, lock held): start the claim once the bus
 *  may be talked on, settle it after its wait, expire the request slots. */
void j1939_tx_tick(int64_t now_us);

/* Frames the receive task sorted (lock held): what active mode answers. */
void j1939_tx_on_claim(uint8_t sa, const uint8_t *name, int64_t now_us);
void j1939_tx_on_request(uint8_t sa, uint8_t da, uint32_t pgn, int64_t now_us);
void j1939_tx_on_ackm(uint8_t sa, uint8_t da, const uint8_t *data, uint8_t dlc,
                      int64_t now_us);

/** Send what the lock-held code queued: the claim frames, the acknowledges,
 *  the transport protocol replies. WITHOUT the lock (a transmit may wait on
 *  the driver). */
void j1939_tx_flush(void);

/** The active-mode fields of the status (lock held). */
void j1939_tx_status(j1939_status_t *out);

/** Zero the active-mode counters (j1939_reset, lock held). */
void j1939_tx_reset(void);

#ifdef __cplusplus
}
#endif
