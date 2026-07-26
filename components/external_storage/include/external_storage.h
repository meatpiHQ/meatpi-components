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
 * @file external_storage.h
 * @brief WiCAN SD/MMC card owner (HAL component, Architecture §4).
 *
 * Owns the SD card as a DEVICE: card-detect supervision (hot-plug), the
 * SDMMC 4-bit host, and mounting the card's FAT filesystem at the `/sd`
 * VFS root. It knows nothing about what is stored — file access goes
 * through the `filesystem` component (`/sd/...` logical paths), which the
 * composition root wires to this component's mount events.
 *
 * Hot-plug: a detect task debounces the card-detect pin; insertion mounts
 * (+ publishes DEV_STATUS_BIT_SDCARD_MOUNTED + fires the event callback),
 * removal unmounts and clears. A yanked card degrades /sd operations to
 * ESP_ERR_INVALID_STATE — never a crash.
 *
 * WiCAN Pro wiring (SDMMC slot, GPIO-matrix routed):
 *   CLK=21 CMD=47 D0=14 D1=13 D2=12 D3=48, card-detect=GPIO40.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount-state change notification. Runs in the detect task — keep it
 *  short (the composition root uses it to flip filesystem's /sd backend). */
typedef void (*external_storage_event_cb_t)(bool mounted);

/** GPIO + log setup; no card access. */
esp_err_t external_storage_init(void);

/** Start the detect task; mounts immediately if a card is present. */
esp_err_t external_storage_start(void);

/** Unmount (if mounted) and stop the detect task. */
esp_err_t external_storage_stop(void);

/** Card physically present (debounced detect pin). */
bool external_storage_is_present(void);

/** Card mounted at /sd right now. */
bool external_storage_is_mounted(void);

/** Register the ONE mount-event callback (composition root). */
esp_err_t external_storage_set_callback(external_storage_event_cb_t cb);

/** Register the `sdcard` CLI command with cmdline_manager. Called
 *  INTERNALLY on the settings boot apply when the `cli` setting is true
 *  (default) — main no longer wires it. */
esp_err_t external_storage_register_cli(void);

/** Register the settings descriptor ({cli}). Init runs before
 *  settings_manager_init, so the composition root calls this separately
 *  (the log_manager_register_settings pattern). */
esp_err_t external_storage_register_settings(void);

#ifdef __cplusplus
}
#endif
