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
 * @file mdns_manager.h
 * @brief WiCAN mDNS advertisement owner (feature component).
 *
 * Rewrite of the legacy `wc_mdns.c` with the ON-AIR CONTRACT the Home
 * Assistant integration discovers by — preserved exactly:
 *
 *  - hostname `wican_<device_id>` (→ `wican_<id>.local`)
 *  - default instance "wican web server"
 *  - service "WiCAN-WebServer" of type **`_wican._tcp` on port 80**
 *  - TXT keys: `mac` (STA MAC, colon-separated uppercase — HA's stable
 *    unique ID), `device_id`, `device_type`, `firmware`, `hardware`,
 *    `version`, `path`
 *
 * One deliberate improvement over legacy: the Pro build passed
 * zero-initialized (empty) version strings — the keys existed with ""
 * values. v6 keeps the SAME keys but fills `firmware`/`version` with
 * the real app version and `hardware` from Kconfig
 * (`WICAN_HW_VERSION`) — strictly more useful to HA, same schema.
 *
 * Device-contract v2 (2026-07-11, ha_webhooks/device-contract): a
 * SECOND service **`_meatpi._tcp` on port 80** is advertised in
 * parallel — the brand-wide discovery surface every MeatPi product
 * shares (integration 3.0 matches only `_meatpi`/`_wican` types). TXT:
 * `device_type` (Kconfig `WICAN_DEVICE_TYPE`, profile slug),
 * `device_id`, `mac`, `fw`, `api`.
 *
 * esp-mdns handles interface up/down itself, so start() works before
 * the network is up; responses begin when an interface appears.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register settings ("mdns_manager") + log descriptors. No network. */
esp_err_t mdns_manager_init(void);

/** Start the responder + advertise the WiCAN service (no-op when
 *  disabled in settings). ESP_ERR_INVALID_STATE when unconfigured. */
esp_err_t mdns_manager_start(void);

/** Withdraw the service and stop the responder. */
esp_err_t mdns_manager_stop(void);

/** "wican_<id>.local" (the legacy wc_mdns_get_hostname). Valid after
 *  start(). */
const char *mdns_manager_hostname(void);

#ifdef __cplusplus
}
#endif
