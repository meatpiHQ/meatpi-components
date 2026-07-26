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
 * @file es_detect.c
 * @brief Pure card-detect debouncer — no GPIO, host-tested.
 */
#include <string.h>

#include "external_storage_private.h"

void es_detect_init(es_detect_t *d, bool initial_present)
{
    memset(d, 0, sizeof(*d));
    d->stable_present = initial_present;
    d->candidate = initial_present;
}

bool es_detect_feed(es_detect_t *d, bool raw_present)
{
    if (raw_present == d->stable_present)
    {
        d->candidate = raw_present;
        d->count = 0; /* any agreement resets a pending change */
        return false;
    }

    if (raw_present != d->candidate)
    {
        d->candidate = raw_present;
        d->count = 1;
        return false;
    }

    d->count++;

    if (d->count < ES_DEBOUNCE_SAMPLES)
    {
        return false;
    }

    d->stable_present = raw_present;
    d->count = 0;
    return true;
}
