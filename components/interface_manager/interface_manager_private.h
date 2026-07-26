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
 * @file interface_manager_private.h
 * @brief Internal contracts: the PURE arbitration policy (host-testable,
 *        no IDF deps) and the config the settings layer fills.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure policy (interface_manager_policy.c) -------------------------------
 * Inputs are the OBSERVED world; output is the desired suspension set.
 * The actuator diffs it against what is currently suspended. Rules
 * (meatpi 2026-07-05, legacy wireless-mode behavior generalized):
 *
 *  R1 sta_ble_handover (legacy "sta+BLE" mode): a BLE client connecting
 *     means the user is in the car -> suspend STA; on BLE disconnect
 *     (back home, phone BLE off) resume STA. BLE keeps advertising.
 *  R2 ap_ble_exclusive: with AP + BLE both enabled they coexist while
 *     IDLE, but the first side a user CONNECTS to wins: BLE client ->
 *     suspend AP; AP station -> suspend (stop) BLE. The loser returns
 *     when the winner disconnects. On a tie BLE wins (the "about to
 *     drive" signal outranks a lingering AP association).
 *
 * Future rules (USB-connected kills both radios, ...) extend the input
 * struct + this function — nothing else changes. */

typedef struct
{
    /* configured surfaces */
    bool mode_has_sta;   /* wifi settings mode includes STA            */
    bool mode_has_ap;    /* wifi settings mode includes AP             */
    bool ble_enabled;    /* ble settings enabled (BLE_ENABLED bit)     */
    /* live signals */
    bool ble_connected;  /* DEV_STATUS_BIT_BLE_CONNECTED               */
    bool ap_has_clients; /* wifi_manager_get_ap_station_count() > 0    */
    /* rule toggles (settings) */
    bool rule_sta_ble_handover;
    bool rule_ap_ble_exclusive;
} im_inputs_t;

typedef struct
{
    bool suspend_sta;
    bool suspend_ap;
    bool suspend_ble;
} im_target_t;

/** Evaluate the rules; pure and total (any input combination). */
void im_policy_evaluate(const im_inputs_t *in, im_target_t *out);

/* ---- settings (interface_manager_settings.c) ----------------------------------- */

typedef struct
{
    bool enabled;
    bool sta_ble_handover;
    bool ap_ble_exclusive;
} im_config_t;

/** Register the "interface_manager" descriptor with settings_manager. */
esp_err_t im_settings_register(void);

/** Boot-applied config; valid once im_settings_is_configured(). */
const im_config_t *im_settings_config(void);
bool im_settings_is_configured(void); /* boot apply ran (standard §4.3) */

#ifdef __cplusplus
}
#endif
