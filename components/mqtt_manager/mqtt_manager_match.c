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
 * @file mqtt_manager_match.c
 * @brief PURE MQTT topic-filter matching + broker-URL validation — no
 *        client, no RTOS; host-testable.
 */
#include <string.h>

#include "mqtt_manager_private.h"

/** Length of the current level (up to '/' or end). */
static size_t level_len(const char *s)
{
    const char *slash = strchr(s, '/');

    return (slash != NULL) ? (size_t)(slash - s) : strlen(s);
}

bool mm_topic_matches(const char *filter, const char *topic)
{
    if (filter == NULL || topic == NULL || filter[0] == '\0')
    {
        return false;
    }

    /* $-topics ($SYS/…) never match a wildcard first level (MQTT spec) */
    if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#'))
    {
        return false;
    }

    while (true)
    {
        size_t flen = level_len(filter);
        size_t tlen = level_len(topic);

        if (flen == 1 && filter[0] == '#')
        {
            /* '#' must be the last level of the filter */
            return filter[1] == '\0';
        }

        if (flen == 1 && filter[0] == '+')
        {
            /* one whole level, any content */
        }
        else
        {
            /* literal level: wildcards embedded in a level are malformed */
            if (memchr(filter, '+', flen) != NULL ||
                memchr(filter, '#', flen) != NULL)
            {
                return false;
            }

            if (flen != tlen || memcmp(filter, topic, flen) != 0)
            {
                return false;
            }
        }

        filter += flen;
        topic += tlen;

        bool f_more = (*filter == '/');
        bool t_more = (*topic == '/');

        if (!f_more && !t_more)
        {
            return true;
        }

        /* "sport/#" also matches "sport" (the parent) */
        if (f_more && !t_more)
        {
            return strcmp(filter, "/#") == 0;
        }

        if (!f_more)
        {
            return false;
        }

        filter++;
        topic++;
    }
}

bool mm_url_valid(const char *url)
{
    if (url == NULL)
    {
        return false;
    }

    const char *host = NULL;

    if (strncmp(url, "mqtt://", 7) == 0)
    {
        host = url + 7;
    }
    else if (strncmp(url, "mqtts://", 8) == 0)
    {
        host = url + 8;
    }

    return host != NULL && host[0] != '\0' && host[0] != ':' &&
           host[0] != '/';
}
