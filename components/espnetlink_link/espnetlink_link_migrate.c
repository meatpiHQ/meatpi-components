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
 * @file espnetlink_link_migrate.c
 * @brief Settings migration for the "espnetlink" descriptor — cJSON
 *        only, no IDF deps, so the host suite exercises every shipped
 *        schema version (standard §5 / §7).
 */
#include "espnetlink_link_migrate.h"

#include "cJSON.h"

static void set_bool(cJSON *o, const char *k, bool v)
{
    cJSON *n = cJSON_CreateBool(v);

    if (cJSON_GetObjectItemCaseSensitive(o, k) != NULL)
    {
        cJSON_ReplaceItemInObjectCaseSensitive(o, k, n);
    }
    else
    {
        cJSON_AddItemToObject(o, k, n);
    }
}

esp_err_t espnl_settings_migrate(uint32_t from_version, cJSON *settings)
{
    if (settings == NULL || !cJSON_IsObject(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (from_version < 2)
    {
        /* v1 shipped enabled=false as its default. A device that never
         * paired (no ssid) just carried that default: give it the v2
         * one so a dongle is plug-and-play after the upgrade. A device
         * with an ssid made a deliberate choice either way — keep it.
         * mode / auto_pair / device_id / cut_retries: schema defaults. */
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(settings,
                                                             "ssid");
        bool has_ssid = cJSON_IsString(ssid) && ssid->valuestring != NULL &&
                        ssid->valuestring[0] != '\0';

        if (!has_ssid)
        {
            set_bool(settings, "enabled", true);
        }
    }

    return ESP_OK;
}
