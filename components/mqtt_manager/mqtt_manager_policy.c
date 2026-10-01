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
 * @file mqtt_manager_policy.c
 * @brief PURE settings policy of mqtt_manager (host-tested, no IDF): the
 *        connect_on parse and the v1 -> v2 migration rule. The IDF glue in
 *        mqtt_manager_settings.c only moves cJSON values through these.
 */
#include <string.h>

#include "mqtt_manager_private.h"

mm_connect_on_t mm_parse_connect_on(const char *value)
{
    return (value != NULL && strcmp(value, "any") == 0) ? MM_CONNECT_ANY
                                                         : MM_CONNECT_WIFI;
}

const char *mm_migrated_connect_on(uint32_t from_version, bool present)
{
    /* a document written before connect_on existed (v1) keeps the v1
       behaviour, any uplink; a document that already carries the key is
       left alone; v2 and later never migrate this key */
    if (from_version < 2 && !present)
    {
        return "any";
    }

    return NULL;
}
