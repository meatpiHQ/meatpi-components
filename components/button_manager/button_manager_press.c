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
 * @file button_manager_press.c
 * @brief Pure press-tracking state machine (host-tested): count
 *        consecutive held ticks, report the long-press exactly once per
 *        press, re-arm on release. No GPIO, no RTOS.
 */
#include "button_manager_private.h"

void btn_press_reset(btn_press_t *p)
{
    p->held_ticks = 0;
    p->fired = false;
}

bool btn_press_step(btn_press_t *p, bool pressed, uint32_t hold_ticks)
{
    if (!pressed)
    {
        btn_press_reset(p); /* release re-arms */
        return false;
    }

    if (p->fired)
    {
        return false; /* still held past the trigger: one shot only */
    }

    p->held_ticks++;

    if (p->held_ticks >= hold_ticks)
    {
        p->fired = true;
        return true;
    }

    return false;
}
