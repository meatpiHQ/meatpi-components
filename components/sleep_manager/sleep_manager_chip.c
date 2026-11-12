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
 * @file sleep_manager_chip.c
 * @brief What the naps see of the OBD chip (2026-11-12). PURE: no IDF
 *        dependency, host-tested.
 *
 *        Every nap reads the chip's READY line. A chip that reads awake is
 *        put back to sleep; that was all, and it left a RAM counter and a W
 *        line that the wake's reboot threw away. Ali, 2026-11-12: "do we
 *        flag if an OBD woke up when it was supposed to be sleeping? Maybe
 *        we should add this a fault code". Two cases are faults, one is
 *        not:
 *
 *          awake before it ever slept in this sleep   put back, no fault.
 *              The entry's sleep word was refused (an app left a monitor
 *              open) and the first re-sleep deals with it: the firmware
 *              coping, not the chip misbehaving.
 *          awake after a nap had found it asleep      put back, and the
 *              fault `obd_chip_woke`. Provisioned as it is (no wake on bus
 *              activity, no voltage or UART wake), a chip has no business
 *              waking while the sleep pin is low: its PowerEn setting is
 *              gone or wrong (a chip whose store was damaged loses it,
 *              issue #1023), or it restarted.
 *          awake SM_RESLEEP_MAX re-sleeps in a row    the fault
 *              `obd_chip_no_sleep` and the recovery reboot. The chip, and
 *              with it the vehicle's battery, is not being put to sleep.
 */
#include "sleep_manager_private.h"

void sm_chip_watch_init(sm_chip_watch_t *w)
{
    w->slept = false;
    w->in_a_row = 0;
    w->wakes = 0;
}

sm_chip_verdict_t sm_chip_watch_feed(sm_chip_watch_t *w, bool awake)
{
    if (!awake)
    {
        w->slept = true;
        w->in_a_row = 0;
        return SM_CHIP_ASLEEP;
    }

    if (w->in_a_row >= SM_RESLEEP_MAX)
    {
        return SM_CHIP_GIVE_UP;
    }

    w->in_a_row++;

    if (w->slept && w->in_a_row == 1)
    {
        w->wakes++; /* once per wake, however many re-sleeps it takes */
        return SM_CHIP_WOKE;
    }

    return SM_CHIP_RESLEEP;
}
