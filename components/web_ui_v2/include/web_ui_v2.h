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
 * @file web_ui_v2.h
 * @brief v2 built-in web UI — registers the embedded single-page app with
 *        http_server_manager's catch-all. No-op unless CONFIG_WICAN_WEBUI_V2
 *        is selected (the Kconfig web-UI choice), so main can always call it.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the v2 asset table (call between http_server_manager_init and
 *  _start). Returns ESP_OK and does nothing if v2 isn't the selected UI. */
esp_err_t web_ui_v2_register(void);

#ifdef __cplusplus
}
#endif
