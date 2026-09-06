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
 * @file ext_manager.h
 * @brief Boot hooks for optional add-on component packs.
 *
 * A build can carry extra component packs through the component-overlay
 * mechanism (see the root CMakeLists: a later component dir overrides an
 * earlier component with the same name). A pack ships its own ext_manager
 * component which wires the pack's features into the firmware's
 * registries at these hook points — bridge jacks
 * (bridge_manager_register_endpoint), the ISO-TP providers
 * (can_isotp_provide, j2534_isotp_provide), settings components, CLI
 * commands, …
 *
 * This stock ext_manager is the no-op default: without a pack the hooks
 * do nothing and the firmware runs with its built-in feature set.
 *
 * Contract with main:
 *   - ext_manager_init()  runs in the init phase, after the core
 *     managers' *_init (settings registration window is still open,
 *     bridge_manager's registry exists, nothing is started yet).
 *   - ext_manager_start() runs in the start phase, after
 *     can_manager_start (bus handle available) and before
 *     bridge_manager pulls its endpoints.
 *   - ext_manager_stop()  runs in the ordered shutdown/sleep-prepare
 *     sequence, after uds_manager_stop and before can_manager_stop.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the pack's settings/jacks/providers. Idempotent. */
esp_err_t ext_manager_init(void);

/** Bring the pack's engines up (CAN is running by now). */
esp_err_t ext_manager_start(void);

/** Stop the pack's engines (sleep prepare / ordered shutdown). */
esp_err_t ext_manager_stop(void);

#ifdef __cplusplus
}
#endif
