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
 * @file autopid_transcript.c
 * @brief PURE: the test-a-PID transcript, one line per exchange, "> cmd" /
 *        "< reply" (2026-09-16: the UI shows what went out and what came
 *        back). Out of autopid_runner.c 2026-10-05 (700-line rule).
 */
#include <string.h>

#include "autopid_private.h"

void ap_tr_add(ap_tr_t *t, char dir, const char *s)
{
    if (t == NULL || t->buf == NULL || t->cap == 0)
    {
        return;
    }

    /* replies are flattened to one line (a multi-frame answer arrives as
       several lines) and capped so a long ISO-TP reply cannot eat the
       whole buffer; the full raw reply travels separately */
    char line[200];
    size_t n = 0;

    line[n++] = dir;
    line[n++] = ' ';

    const char *p = s;

    for (; *p != '\0' && n < sizeof(line) - 5; p++)
    {
        if (*p == '\r' || *p == '\n')
        {
            if (line[n - 1] != ' ')
            {
                line[n++] = ' ';
            }
        }
        else
        {
            line[n++] = *p;
        }
    }

    while (n > 2 && line[n - 1] == ' ')
    {
        n--;
    }

    if (*p != '\0')
    {
        line[n++] = '.';
        line[n++] = '.';
        line[n++] = '.';
    }

    line[n++] = '\n';
    line[n] = '\0';

    if (t->len + n < t->cap)
    {
        memcpy(t->buf + t->len, line, n + 1);
        t->len += n;
    }
}
