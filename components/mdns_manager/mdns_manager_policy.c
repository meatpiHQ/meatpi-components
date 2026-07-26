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
 * @file mdns_manager_policy.c
 * @brief PURE string builders for the mDNS contract — host-testable.
 */
#include <stdio.h>

#include "mdns_manager_private.h"

bool mm_format_mac(const uint8_t mac[6], char *out, size_t cap)
{
    if (mac == NULL || out == NULL)
    {
        return false;
    }

    return snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                    mac[1], mac[2], mac[3], mac[4],
                    mac[5]) < (int)cap;
}

bool mm_build_hostname(const char *id, char *out, size_t cap)
{
    if (id == NULL || id[0] == '\0' || out == NULL)
    {
        return false;
    }

    return snprintf(out, cap, "wican_%s", id) < (int)cap;
}

bool mm_build_hostname_local(const char *id, char *out, size_t cap)
{
    if (id == NULL || id[0] == '\0' || out == NULL)
    {
        return false;
    }

    return snprintf(out, cap, "wican_%s.local", id) < (int)cap;
}
