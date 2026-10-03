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
 * @file autopid_poller.h
 * @brief What autopid.c (lifecycle, tables, public API) and
 *        autopid_poller.c (the task) share. Private to those two files:
 *        everybody else goes through autopid_private.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "autopid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- autopid.c, read by the task ------------------------------------------- */

/** The `enabled` setting as it stands now (autopid_stop() clears it). */
bool ap_core_enabled(void);

/** The counters behind autopid_stats(): the task owns `running`, the poll
 *  counts and `last_poll_us`; the config loader owns the `*_loaded` ones. */
autopid_stats_t *ap_core_stats(void);

/* ---- autopid_poller.c ------------------------------------------------------- */

/** Create the poller task. @p batt_q carries the voltage-pause events
 *  (battery_monitor_event_t), NULL = no voltage pause. Once. */
void ap_poller_start(QueueHandle_t batt_q);

/** Fill the `paused_*` flags of @p out. */
void ap_poller_pauses(autopid_stats_t *out);

/** esp_timer time of the last successful ECU poll, 0 = never. */
int64_t ap_poller_last_ok_us(void);

#ifdef __cplusplus
}
#endif
