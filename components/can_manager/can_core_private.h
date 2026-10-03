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
 * @file can_core_private.h
 * @brief What can_core.c (RX task, dispatch, TX, stats) and can_core_link.c
 *        (the node's link to the bus: listen before talk, the transmit
 *        gate) share. Private to those two files.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "can_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM

/* ---- can_core.c --------------------------------------------------------- */

/** Rendezvous with the RX task before a driver bounce from ANOTHER task:
 *  the caller sets handle->reconfiguring, then waits here until the task
 *  is provably outside a receive. */
void can_core_rx_wait_parked(can_core_handle_t *handle);

/* ---- can_core_link.c ------------------------------------------------------ */

/** Start the listen-before-talk policy and the node (always listen-only,
 *  no TX pin). Called by can_core_init() once the raw queue exists. */
elm327_err_t can_link_start(can_core_handle_t *handle);

/** Step the policy and apply what it decides (a node bounce). RX-task
 *  context only: the task is by definition outside a receive here. */
void can_link_service(can_core_handle_t *handle);

/** How long the RX task may block before the policy wants another look. */
uint32_t can_link_poll_ms(const can_core_handle_t *handle);

/** Transmit gate: true = the caller may hand ONE frame to the driver and
 *  must call can_link_tx_exit() after. false = the node may not talk
 *  within @p timeout_ms (counted in stats.tx_refused). */
bool can_link_tx_enter(can_core_handle_t *handle, uint32_t timeout_ms);
void can_link_tx_exit(can_core_handle_t *handle);

/** Close the gate and wait until no transmit is inside the driver. */
void can_link_tx_close(can_core_handle_t *handle);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
