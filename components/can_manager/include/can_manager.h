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
 * @file can_manager.h
 * @brief Owner of the native CAN (TWAI) bus — WiCAN Pro TX=GPIO2 /
 *        RX=GPIO1 / STDBY=GPIO38.
 *
 * Model (ARCHITECTURE "the one pattern"): can_manager owns the ONE
 * physical CAN peripheral through a single shared `can_core` handle;
 * every consumer REGISTERS into it instead of touching the driver:
 *   - AT-engine clients (per-engine filter/mask — add-on packs),
 *   - task-owned RX queues (drop-oldest fan-out) via
 *     can_manager_subscribe_queue(),
 *   - plain TX via can_manager_send().
 *
 * Settings ("can_manager", reboot-to-apply): enabled (default false),
 * baud (kbit/s enum), silent (bus-wide listen-only), cli.
 *
 * The ESP TWAI transceiver and the MIC3624 OBD chip are two nodes on the
 * SAME vehicle CAN bus — electrically legal; contention between autopid
 * polling (via the MIC) and native-CAN traffic is a policy question left
 * to the user in v1 (README).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "can_core.h" /* the shared-bus core (adopted, see PROVENANCE) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool     enabled;      /**< settings value                              */
    bool     running;      /**< bus up (driver installed + started)         */
    bool     silent;       /**< bus-wide listen-only mode                   */
    uint32_t baud_kbps;
    can_core_stats_t stats;
} can_manager_status_t;

/** Register settings + log descriptors. Call in main's init pass. */
esp_err_t can_manager_init(void);

/** Bring the bus up per settings (no-op when disabled). §4 lifecycle. */
esp_err_t can_manager_start(void);

/** Park the bus: TWAI stopped/uninstalled, transceiver into standby.
 *  Called by main's sleep-prepare BEFORE sleep_manager's GPIO hold. */
esp_err_t can_manager_stop(void);

/** The shared bus handle for the adopted elm327_* components.
 *  NULL while the bus is not running. */
struct can_core_handle_s *can_manager_core_handle(void);

/** Thin TX wrapper for v6-native consumers. ESP_ERR_INVALID_STATE when
 *  the bus is down. */
esp_err_t can_manager_send(uint32_t id, bool ext, bool rtr,
                           const uint8_t *data, uint8_t dlc);

/** Subscribe a task-owned queue of can_core_frame_t (drop-oldest when
 *  full). monitor_all=true bypasses filter/mask. Returns the subscriber
 *  index via out_idx (>=0) for later unsubscribe. */
esp_err_t can_manager_subscribe_queue(QueueHandle_t q, uint32_t filter,
                                      uint32_t mask, bool ext,
                                      bool monitor_all, int *out_idx);

esp_err_t can_manager_unsubscribe_queue(int idx);

esp_err_t can_manager_status(can_manager_status_t *out);

/** Zero the stats counters (CLI `can -z`). */
void can_manager_zero_stats(void);

/** /api/can route (HTTP compositions only — main wires it). */
esp_err_t can_manager_register_http(void);

#ifdef __cplusplus
}
#endif
