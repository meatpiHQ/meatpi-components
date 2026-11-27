/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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

/* obd_gate's bus lock (2026-11-27): while a script engine ECU flash holds
 * it, the chip's request and raw-send paths refuse and nothing reaches the
 * wire; released, they work again. */
#include "obd_chip.h"

#include <string.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"

#define T_AT pdMS_TO_TICKS(500)

void rq_engine_up(void);
void rq_wire_clear(void);
const char *rq_wire(void);

extern bool test_obd_bus_locked;
extern unsigned test_obd_bus_refusals;

void test_bus_lock_refuses_request_and_send(void)
{
    char resp[64] = "untouched";
    static const uint8_t ATI[] = "ATI\r";

    rq_engine_up();
    rq_wire_clear();
    test_obd_bus_refusals = 0;
    test_obd_bus_locked = true;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      obd_chip_request("0100", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("", resp);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, obd_chip_send(ATI, 4));
    TEST_ASSERT_EQUAL_STRING("", rq_wire());
    TEST_ASSERT_EQUAL_UINT(2, test_obd_bus_refusals);

    test_obd_bus_locked = false;
    TEST_ASSERT_EQUAL(ESP_OK, obd_chip_request("0100", resp, sizeof(resp), T_AT));
    TEST_ASSERT_EQUAL_STRING("0100\r", rq_wire());
}
