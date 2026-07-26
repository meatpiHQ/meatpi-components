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
 * @file usb_host_manager_presence.c
 * @brief Pure ID-pin debouncer (host-tested; no esp deps). Presence =
 *        pin LOW (the OTG cable grounds ID).
 */
#include "usb_host_manager_private.h"

void uhm_presence_init(uhm_presence_t *p, int initial_level)
{
    p->present = (initial_level == 0);
    p->candidate = p->present;
    p->count = 0;
}

uhm_edge_t uhm_presence_sample(uhm_presence_t *p, int level)
{
    bool raw = (level == 0);

    if (raw == p->present)
    {
        p->candidate = p->present;
        p->count = 0;
        return UHM_EDGE_NONE;
    }

    if (raw != p->candidate)
    {
        p->candidate = raw;
        p->count = 1;
        return UHM_EDGE_NONE;
    }

    if (++p->count < UHM_PRESENCE_STABLE_SAMPLES)
    {
        return UHM_EDGE_NONE;
    }

    p->present = raw;
    p->count = 0;
    return raw ? UHM_EDGE_ATTACH : UHM_EDGE_DETACH;
}
