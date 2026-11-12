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
 * @file obd_chip_reset_line.h
 * @brief The OBD chip's reset line: one function, for one caller.
 *
 *        A reset inside a boot of the chip can destroy the chip for good
 *        (issue #1023). So the line is the LAST RESORT, for a chip that has
 *        just been asked at both UART speeds and answered at neither, and it
 *        is in no API: not in obd_chip.h, not on HTTP, not on the console,
 *        and not in obd_chip_private.h either. This header is included by
 *        obd_chip_pins.c (the function) and obd_chip_bringup.c (its two
 *        calls: the bring-up's last resort and the end of a chip update)
 *        and by NOTHING ELSE. The host tests count that
 *        (test_reset_line_has_one_driver_and_two_last_resort_calls).
 *
 *        Need a reset somewhere else? Send the chip a command
 *        (obd_chip_soft_reset(), `ATWS`, `ATZ`): it runs a command from its
 *        own main loop, never inside its own boot.
 */
#pragma once

#include <stdbool.h>

/**
 * RESET low 5 ms then high, as the last resort.
 *
 * @param chip_silent the caller's own finding, made just before the call:
 *                    the chip was asked at both UART speeds and gave no
 *                    prompt at either. Pass the result of that look, never a
 *                    constant.
 * @return true = the pulse went out; false = refused (the chip was not
 *         found silent, or READY never came to rest), counted, logged with
 *         one W line, the line untouched.
 *
 * Waits for READY to settle first (OBD_PULSE_SETTLE_MS at most, so it
 * returns 0.3 to 2.5 s after the call) and asks obd_settle_pulse_allowed().
 */
bool obd_pin_reset_pulse(bool chip_silent);
