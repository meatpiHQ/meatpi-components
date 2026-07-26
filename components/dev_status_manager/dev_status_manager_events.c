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
 * @file dev_status_manager_events.c
 * @brief event_manager glue: `status.bit {bit, set}` on EVERY bit
 *        change — mqtt/sd/time_synced/sta_connected/ble_connected/…
 *        rules for free (the legacy HOME/DRIVE-mode idea, generalized:
 *        contexts are whatever rules make of the bits).
 */
#include <stdio.h>

#include "event_manager.h"

#include "dev_status_manager.h"

static void on_change(EventBits_t changed, EventBits_t now)
{
    /* one event per changed bit (usually exactly one) */
    for (int i = 0; i < 24; i++)
    {
        EventBits_t bit = (EventBits_t)1 << i;

        if ((changed & bit) == 0)
        {
            continue;
        }

        em_event_t ev = { 0 };

        snprintf(ev.source, sizeof(ev.source), "status");
        snprintf(ev.name, sizeof(ev.name), "bit");
        ev.kv[0] = em_kv_str("bit", dev_status_manager_bit_name(bit));
        ev.kv[1] = em_kv_bool("set", (now & bit) != 0);
        ev.n = 2;
        (void)event_manager_publish(&ev);
    }
}

void dsm_events_register(void)
{
    static const em_key_decl_t KEYS[] =
    {
        { "bit", EM_VAL_STR },
        { "set", EM_VAL_BOOL },
    };
    static const em_source_decl_t BIT =
    {
        .source = "status", .name = "bit",
        .description = "a device-status bit changed (sta_connected, "
                       "ble_connected, mqtt_connected, motion, …)",
        .keys = KEYS, .n_keys = 2,
    };

    (void)event_manager_declare_source(&BIT);
    (void)dev_status_manager_subscribe_changes(on_change);
}
