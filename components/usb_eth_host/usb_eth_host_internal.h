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

#pragma once

#include "usb_eth_host.h"

#ifdef __cplusplus
extern "C" {
#endif

void usb_eth_host_notify_driver_started(usb_eth_host_driver_t driver, const char *ifkey);
void usb_eth_host_notify_driver_stopped(usb_eth_host_driver_t driver);

/* Called by the netif glue right before it destroys a netif so the cached
 * g_last_usb_eth_netif pointer never outlives the netif. Without this, a
 * WiFi STA got-ip event after a USB detach dereferences the destroyed
 * netif in usb_eth_host_on_sta_got_ip (LoadProhibited — bench-hit
 * 2026-07-30, ESPNetLink full-system GPS test). */
void usb_eth_host_notify_netif_destroyed(esp_netif_t *netif);

#ifdef __cplusplus
}
#endif