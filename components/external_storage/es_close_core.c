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
 * @file es_close_core.c
 * @brief Pure rule of the close guard (es_close.c, 2026-11-14): when a
 *        close that failed has to be made again, and with which status
 *        flags. No IDF or FatFs dependency: host-tested.
 */
#include "external_storage_private.h"

bool es_close_retry(bool first_ok, bool still_bound, uint8_t *flag)
{
    if (first_ok || !still_bound || flag == NULL)
    {
        /* closed, or FatFs no longer knows the object (an unmounted card,
           a stale handle): nothing a second close could release */
        return false;
    }

    /* the flush is what failed and what a second close would try again:
       without these two bits FatFs has nothing to write and goes straight
       to the release of the lock */
    *flag &= (uint8_t)~(ES_FA_MODIFIED | ES_FA_DIRTY);
    return true;
}
