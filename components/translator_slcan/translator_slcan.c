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
 * @file translator_slcan.c
 * @brief Registration (the one target-only edge; the codec lives in
 *        translator_slcan_codec.c and is pure/host-tested).
 */
#include "translator_slcan.h"

#include "esp_log.h"

#include "bridge_manager.h"

extern const bridge_translator_t translator_slcan_desc;

esp_err_t translator_slcan_init(void)
{
    esp_err_t err = bridge_manager_register_translator(&translator_slcan_desc);

    if (err == ESP_OK)
    {
        ESP_LOGI("translator_slcan", "registered 'slcan' codec");
    }
    return err;
}
