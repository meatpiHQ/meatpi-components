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
 * @file usb_acm_gps.c
 * @brief Pure parser for the ESPNetLink `gps -p -j` response. No deps
 *        (no cJSON) — the dongle emits a FLAT JSON object, so a keyed
 *        numeric scan is enough and it stays host-testable.
 */
#include "usb_acm_gps.h"

#include <stdlib.h>
#include <string.h>

/* Find `"key":` and read the number after it. The leading quote makes the
 * key unambiguous — `"lat":` never matches inside `"cached_lat":`. */
static bool find_num(const char *json, const char *quoted_key, double *out)
{
    const char *p = strstr(json, quoted_key);

    if (p == NULL)
    {
        return false;
    }

    p += strlen(quoted_key);

    while (*p == ' ' || *p == '\t')
    {
        p++;
    }

    char *end = NULL;
    double v = strtod(p, &end);

    if (end == p)
    {
        return false;
    }

    *out = v;
    return true;
}

static bool find_true(const char *json, const char *quoted_key)
{
    const char *p = strstr(json, quoted_key);

    if (p == NULL)
    {
        return false;
    }

    p += strlen(quoted_key);

    while (*p == ' ' || *p == '\t')
    {
        p++;
    }

    return *p == 't'; /* true / false */
}

bool usb_acm_gps_parse(const char *json, usb_acm_gps_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (json == NULL)
    {
        return false;
    }

    /* skip to the first object (past the command echo) */
    const char *obj = strchr(json, '{');

    if (obj == NULL)
    {
        return false;
    }

    if (!find_true(obj, "\"valid\":"))
    {
        return false; /* no live fix — do not report a cached position */
    }

    double d;

    if (!find_num(obj, "\"lat\":", &out->latitude) ||
        !find_num(obj, "\"lon\":", &out->longitude))
    {
        return false; /* a "valid" fix without coordinates is unusable */
    }

    if (find_num(obj, "\"altitude_m\":", &d))
    {
        out->altitude_m = d;
    }

    if (find_num(obj, "\"speed_kmph\":", &d))
    {
        out->speed_kmph = d;
    }

    if (find_num(obj, "\"course_deg\":", &d))
    {
        out->heading_deg = d;
    }

    if (find_num(obj, "\"satellites\":", &d))
    {
        out->satellites = (int)d;
    }

    if (find_num(obj, "\"hdop\":", &d))
    {
        /* accuracy ~= HDOP × consumer-GPS URE (~5 m); hdop is >= 0 so a
         * +0.5 truncation rounds correctly without libm */
        out->accuracy_m = (int)(d * 5.0 + 0.5);
    }

    out->valid = true;
    return true;
}
