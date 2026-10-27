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
 * @file j2534_server_migrate.c
 * @brief Pure settings migration for j2534_server (no IDF deps beyond
 *        cJSON and esp_err_t, so the host suite runs it).
 *
 * v1 -> v2 (2026-10-27): `allow_reflash` goes. The ECU-flashing content
 * gate refused ordinary raw CAN frames of a VW TP 2.0 flasher by reading
 * them as ISO-TP (field report 2026-10-27) and blocked ISO15765 memory
 * reads while missing every other write service; the device now forwards
 * whatever a tool writes and counts the memory-transfer services it saw
 * (/api/j2534 reflash_seen). Nothing else in the object changes.
 */
#include "j2534_server_migrate.h"

esp_err_t j2534_settings_migrate(uint32_t from_version, cJSON *settings)
{
    if (settings == NULL || !cJSON_IsObject(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (from_version < 2)
    {
        /* absent in a v1 object that never had it: nothing to delete */
        cJSON_DeleteItemFromObjectCaseSensitive(settings, "allow_reflash");
    }

    return ESP_OK;
}
