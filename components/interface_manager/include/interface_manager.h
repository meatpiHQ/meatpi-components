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
 * @file interface_manager.h
 * @brief WiCAN wireless-interface arbitration (policy component).
 *
 * Owns the RULES for which radio interface is active when (meatpi
 * 2026-07-05, generalizing the legacy wireless modes):
 *
 *  - sta_ble_handover: a BLE client connecting means the user is in the
 *    car — STA is suspended for the drive; when the BLE client leaves
 *    (back home, phone BLE off) STA reconnects and BLE keeps
 *    advertising. (The legacy "sta+BLE" mode.)
 *  - ap_ble_exclusive: AP and BLE both advertise while idle; the side
 *    the user connects to wins — a BLE client suspends the AP, an AP
 *    station stops BLE. The loser returns when the winner disconnects.
 *    BLE wins a tie.
 *
 * Future rules (e.g. USB-connected turns both radios off) land here —
 * this component is the one place interface on/off decisions live.
 *
 * Mechanics: a small task polls the dev-status bits + AP station count
 * (debounced), evaluates the PURE rule table, and diffs the result
 * against the current suspensions — actuating via wifi_manager's
 * runtime suspend/resume and ble_manager stop/start. Everything is
 * EPHEMERAL: settings are never touched, a reboot restores the
 * configured baseline.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register settings ("interface_manager") + log descriptors. */
esp_err_t interface_manager_init(void);

/** Start the arbitration task. ESP_ERR_INVALID_STATE when unconfigured;
 *  ESP_OK (idle) when disabled in settings. */
esp_err_t interface_manager_start(void);
esp_err_t interface_manager_stop(void);

#ifdef __cplusplus
}
#endif
