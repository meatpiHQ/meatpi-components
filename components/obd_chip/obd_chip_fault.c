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
 * @file obd_chip_fault.c
 * @brief What a bring-up of the chip leaves behind as a device fault code
 *        (2026-11-12). PURE: no IDF dependency, host-tested; obd_chip.c
 *        latches the result with dev_status_manager.
 *
 *        Until this file a chip that answered nothing was a few log lines
 *        and `ready: false` on GET /api/obd_chip, all of it gone at the
 *        next boot: the bench unit whose chip was destroyed reproducing
 *        issue #1023 had an empty fault list. A fault code stays until it
 *        is cleared by hand, so a chip that was silent once in the field is
 *        still on record when the unit comes back.
 *
 *          obd_chip_silent   the chip answered at neither UART speed and
 *                            the bring-up took its last resort, the reset
 *                            line. The detail says how that ended. Latched
 *                            also when the chip came back: the reset line
 *                            is never used on a healthy chip, so its use
 *                            is the fact worth keeping.
 *          obd_chip_damaged  the chip runs, but answers `VTVERS` with an
 *                            error: its stored identity is erased (a reset
 *                            inside its own store housekeeping).
 */
#include <string.h>

#include "obd_chip_private.h"

bool obd_fault_silent(bool silent, bool pulsed, bool found, char *detail,
                      size_t detail_len)
{
    if (!silent || detail == NULL || detail_len == 0)
    {
        return false;
    }

    const char *text = !pulsed ? "silent; READY never at rest, no reset pulse"
                       : found ? "silent; came back after the reset pulse"
                               : "silent; still silent after the reset pulse";

    strncpy(detail, text, detail_len - 1);
    detail[detail_len - 1] = '\0';
    return true;
}
