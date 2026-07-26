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
 * @file wifi_manager_bridge.h
 * @brief Component-private bridge between wifi_manager.c (owns the state)
 *        and wifi_manager_status.c (public status/scan surface). Kept out of
 *        wifi_manager_private.h so the pure selection module stays IDF-free.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wm_status_copy_ip(char *buf, size_t buf_len);

/** which: 0 = enabled, 1 = STA connected, 2 = AP started. */
bool wm_status_flag(int which);

uint16_t wm_status_ap_clients(void);
esp_netif_t *wm_sta_netif(void);
esp_netif_t *wm_ap_netif(void);
EventGroupHandle_t wm_event_group(void);
void wm_set_callbacks(const wifi_manager_callbacks_t *cbs);

/**
 * Blocking scan under the component lock. On ESP_OK the records stay valid
 * until wm_scan_unlock(), which the caller MUST invoke (it also restores an
 * AP-only radio mode switched for the scan).
 */
esp_err_t wm_scan_locked(wifi_ap_record_t **records, uint16_t *count);
void wm_scan_unlock(void);

#ifdef __cplusplus
}
#endif
