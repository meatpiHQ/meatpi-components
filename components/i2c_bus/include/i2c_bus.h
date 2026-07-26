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
 * @file i2c_bus.h
 * @brief WiCAN shared I2C bus owner (HAL component, Architecture §4).
 *
 * Owns THE one I2C master bus (WiCAN Pro: SDA=GPIO5, SCL=GPIO6 — Kconfig
 * per hardware revision) that the on-board peripherals share: the AW2023
 * LED controller (led_manager), the RX8130 RTC (rtc_manager) and the
 * ICM-42670 IMU (imu_manager). Peripheral managers attach their device
 * handles here (`i2c_bus_add_device`); they never create a bus.
 *
 * Thread safety: the IDF v6 `i2c_master` driver serializes TRANSACTIONS on
 * the bus with its own per-bus lock, so managers on different tasks can
 * talk to their devices concurrently. A manager whose operation spans
 * MULTIPLE transactions (read-modify-write of a register) still needs its
 * own mutex around the sequence — that's the manager's job, not this
 * component's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the master bus (no device traffic). */
esp_err_t i2c_bus_init(void);

/** Lifecycle uniformity (§3); passive component — both trivial. */
esp_err_t i2c_bus_start(void);
esp_err_t i2c_bus_stop(void);

/** The one bus handle; NULL before init. */
i2c_master_bus_handle_t i2c_bus_handle(void);

/** Attach a 7-bit device at @p addr / @p speed_hz (thin wrapper so
 *  managers don't repeat the config struct). */
esp_err_t i2c_bus_add_device(uint8_t addr, uint32_t speed_hz,
                             i2c_master_dev_handle_t *out);

/** ACK-probe @p addr (diagnostics; used by the bench + /api/status). */
esp_err_t i2c_bus_probe(uint8_t addr);

#ifdef __cplusplus
}
#endif
