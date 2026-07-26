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
 * @file script_engine_name.c
 * @brief Script-name validation — a PURE guard used before opening a file
 *        under /data/scripts (script_engine.c load path + the script.run
 *        event action). Split out of script_engine.c so it can be
 *        host-tested without the Berry VM / filesystem dependencies.
 *
 * Rejects anything that could escape the scripts directory or isn't a
 * plain filename: empty, over-long, non-[alnum _ - .] characters, or a
 * ".." path-traversal sequence.
 */
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

/* declared in script_engine_private.h (which pulls in berry.h etc.); this
 * file stays free of target deps so the host test can compile it alone. */
bool se_script_name_ok(const char *name);

bool se_script_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > 40)
    {
        return false;
    }

    for (const char *p = name; *p != '\0'; p++)
    {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' &&
            *p != '.')
        {
            return false;
        }
    }

    return strstr(name, "..") == NULL;
}
