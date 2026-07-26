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
 * @file sleep_manager_events.c
 * @brief event_manager glue: `sleep.entering {volts}` (published
 *        BEFORE teardown so rules get a final MQTT/HTTP shot) and
 *        `sleep.state {state, volts}` transitions.
 */
#include <stdio.h>

#include "event_manager.h"

#include "sleep_manager_private.h"

void sm_events_register(void)
{
    static const em_key_decl_t ENTER_KEYS[] =
    {
        { "volts", EM_VAL_F64 },
    };
    static const em_source_decl_t ENTERING =
    {
        .source = "sleep", .name = "entering",
        .description = "the device is about to power down into light "
                       "sleep; last chance for rules to publish (fired "
                       "BEFORE radios stop)",
        .keys = ENTER_KEYS, .n_keys = 1,
    };
    static const em_key_decl_t STATE_KEYS[] =
    {
        { "state", EM_VAL_STR },
        { "volts", EM_VAL_F64 },
    };
    static const em_source_decl_t STATE =
    {
        .source = "sleep", .name = "state",
        .description = "sleep ladder transition "
                       "(normal/low_voltage/sleeping/wake_pending)",
        .keys = STATE_KEYS, .n_keys = 2,
    };

    (void)event_manager_declare_source(&ENTERING);
    (void)event_manager_declare_source(&STATE);
}

void sm_events_entering(float volts)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "sleep");
    snprintf(ev.name, sizeof(ev.name), "entering");
    ev.kv[0] = em_kv_f64("volts", (double)volts);
    ev.n = 1;
    (void)event_manager_publish(&ev);
}

void sm_events_state(const char *state, float volts)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "sleep");
    snprintf(ev.name, sizeof(ev.name), "state");
    ev.kv[0] = em_kv_str("state", state);
    ev.kv[1] = em_kv_f64("volts", (double)volts);
    ev.n = 2;
    (void)event_manager_publish(&ev);
}
