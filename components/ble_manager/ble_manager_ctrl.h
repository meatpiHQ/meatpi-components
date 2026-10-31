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
 * @file ble_manager_ctrl.h
 * @brief The ESP32-S3 controller's sizing from the Bluetooth memory profile
 *        (settings v5): the one place both GATT backends size the
 *        controller config before esp_bt_controller_init(). Target only
 *        (esp_bt.h); the pure mapping is in ble_manager_private.h.
 */
#pragma once

#include "esp_bt.h"

#include "ble_manager_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Size @p bt_cfg (a BT_CONTROLLER_INIT_CONFIG_DEFAULT() the backend built,
 *  its own task sizing already applied) per the boot-applied memory
 *  profile, log the result once and keep it for blm_ctrl_applied(). Call
 *  right before esp_bt_controller_init(). */
void blm_ctrl_configure(esp_bt_controller_config_t *bt_cfg);

/** The sizing applied at the last stack bring-up; NULL before the first. */
const blm_ram_cfg_t *blm_ctrl_applied(void);

#ifdef __cplusplus
}
#endif
