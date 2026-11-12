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
 * @file obd_chip_settle.c
 * @brief When the chip's READY line has settled (issue #1023, 2026-11-12).
 *        PURE: no IDF dependency; the host tests feed it the traces the
 *        bench measured.
 *
 *        Why it exists. The chip tidies its stored settings in the first
 *        boot behind every 264 writes of its store, from about 68 ms into
 *        that boot, for about 155 ms. A reset in that time destroys the
 *        store: the chip loses its identity and its PowerEn setting, or it
 *        never answers again. Two bench chips were killed that way, and both
 *        field signatures of the issue reproduced. The firmware cannot see
 *        the tidying, but it can see a boot: READY is not ready while the
 *        chip boots, tidying included. So no reset goes out, and no bring-up
 *        decides anything, until READY has held one level long enough that
 *        no boot can be running.
 *
 *        READY, sampled every millisecond on two units (ms behind the
 *        event; L = ready, H = not ready):
 *          sleep pin high, chip asleep    H0 L113 H231..237 L497..502
 *          the same, store to tidy        H0 L113 H231 L649..678
 *          reset pulse, chip awake        L0 H19 L285
 *          the same, store to tidy        L0 H19 L412..459
 *        The longest "not ready" of a boot is about 450 ms; the "ready" of a
 *        waking chip BEFORE it restarts itself lasts 117 to 123 ms. Hence
 *        the two times in obd_chip_private.h.
 *
 *        The rule of the reset line (Ali, 2026-11-12: "if the reset pin is
 *        what drives it, let's make sure it's never used", then "the pin
 *        reset should not be allowed unless as a last resort"). Waiting for
 *        READY keeps a pulse out of every boot that was measured, and the
 *        chip's firmware is not ours to read: so the line is left to the one
 *        job nothing else does, a chip that answers nothing. It has been
 *        woken by the sleep pin, READY has been waited for, and it was asked
 *        at both UART speeds: only then, and only with READY at rest. A chip
 *        that talks is reset by command (`ATWS`, `ATZ`), which it runs from
 *        its own main loop and so never inside its own tidying.
 *        obd_settle_pulse_allowed() is that rule; the one function that
 *        drives the line (obd_chip_pins.c, declared in
 *        obd_chip_reset_line.h) asks it before every pulse and has no way
 *        round.
 */
#include "obd_chip_private.h"

void obd_settle_reset(obd_settle_t *s)
{
    s->seen = false;
    s->ready = false;
    s->since_us = 0;
}

obd_settle_verdict_t obd_settle_feed(obd_settle_t *s, bool ready, int64_t now_us)
{
    if (!s->seen || ready != s->ready)
    {
        s->seen = true;
        s->ready = ready;
        s->since_us = now_us; /* the level just changed: count from here */
    }

    const int64_t steady_us = now_us - s->since_us;

    if (s->ready && steady_us >= (int64_t)OBD_READY_STEADY_MS * 1000)
    {
        return OBD_SETTLE_READY;
    }

    if (!s->ready && steady_us >= (int64_t)OBD_DOWN_STEADY_MS * 1000)
    {
        return OBD_SETTLE_DOWN;
    }

    return OBD_SETTLE_MOVING;
}

bool obd_settle_pulse_allowed(obd_settle_verdict_t settled, bool chip_silent)
{
    /* the last resort: a chip that was asked at both UART speeds and
       answered at neither, and READY at rest (ready, or not ready, for long
       enough that no boot can be running). READY reading "asleep" alone is
       not enough, and a line that moves gets no pulse, whoever asks. */
    return chip_silent && settled != OBD_SETTLE_MOVING;
}
