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
 * @file interface_manager_policy.c
 * @brief PURE wireless arbitration rules — no IDF deps; host-tested.
 *        See interface_manager_private.h for the rule definitions.
 */
#include "interface_manager_private.h"

void im_policy_evaluate(const im_inputs_t *in, im_target_t *out)
{
    out->suspend_sta = false;
    out->suspend_ap = false;
    out->suspend_ble = false;

    if (!in->ble_enabled)
    {
        return; /* every rule involves BLE (for now) */
    }

    /* R1: BLE client present -> the user is in the car; STA yields */
    if (in->rule_sta_ble_handover && in->mode_has_sta && in->ble_connected)
    {
        out->suspend_sta = true;
    }

    /* R2: AP vs BLE — whoever the user connects to wins; BLE wins ties */
    if (in->rule_ap_ble_exclusive && in->mode_has_ap)
    {
        if (in->ble_connected)
        {
            out->suspend_ap = true;
        }
        else if (in->ap_has_clients)
        {
            out->suspend_ble = true;
        }
    }
}
