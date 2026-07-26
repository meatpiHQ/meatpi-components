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
 * @file http_server_manager_auth.c
 * @brief PURE admin-password check (host-tested): parse the request's
 *        Authorization header (Basic — any username, the password part
 *        counts — or Bearer) or the `wican_auth` cookie and compare
 *        against the configured password. No httpd, no settings — the
 *        caller feeds strings. Comparison is flat (no early-out) so
 *        timing doesn't leak the match length position.
 *
 * Design (meatpi 2026-07-19, supersedes the parked pairing-token
 * sketch): ONE device password, disabled by default; when enabled every
 * inbound HTTP request and WS handshake must present it. Basic auth
 * means a stock browser works with zero UI code (native prompt, cached
 * credentials ride every request including the WS upgrade); the cookie
 * is the fallback for clients that can't set headers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "http_server_manager_private.h"

/* tiny base64 decoder — dependency-free so this file host-tests clean */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/** Decode @p in into @p out (NUL-terminated). @return decoded length,
 *  or -1 on bad input / overflow. */
static int b64_decode(const char *in, char *out, size_t out_len)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;

    for (; *in != '\0' && *in != '='; in++)
    {
        int v = b64_val(*in);

        if (v < 0)
        {
            return -1;
        }

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;

            if (n + 1 >= out_len)
            {
                return -1;
            }

            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }

    out[n] = '\0';
    return (int)n;
}

/** Flat comparison: every byte participates regardless of mismatches. */
static bool str_equal_flat(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = (la > lb) ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);

    for (size_t i = 0; i < n; i++)
    {
        char ca = (i < la) ? a[i] : 0;
        char cb = (i < lb) ? b[i] : 0;

        diff |= (unsigned)(ca ^ cb);
    }

    return diff == 0;
}

bool hsm_auth_check(const char *auth_hdr, const char *cookie_hdr,
                    const char *password)
{
    if (password == NULL || password[0] == '\0')
    {
        return true; /* no password configured -> open */
    }

    if (auth_hdr != NULL)
    {
        if (strncmp(auth_hdr, "Bearer ", 7) == 0)
        {
            if (str_equal_flat(auth_hdr + 7, password))
            {
                return true;
            }
        }
        else if (strncmp(auth_hdr, "Basic ", 6) == 0)
        {
            char creds[160];

            if (b64_decode(auth_hdr + 6, creds, sizeof(creds)) > 0)
            {
                const char *colon = strchr(creds, ':');

                /* any username; the password part decides */
                if (colon != NULL && str_equal_flat(colon + 1, password))
                {
                    return true;
                }
            }
        }
    }

    if (cookie_hdr != NULL)
    {
        const char *p = cookie_hdr;

        while ((p = strstr(p, "wican_auth=")) != NULL)
        {
            /* must be the start of a cookie-pair, not a substring */
            if (p != cookie_hdr && p[-1] != ' ' && p[-1] != ';')
            {
                p += 11;
                continue;
            }

            char val[80];
            size_t n = 0;

            p += 11;

            while (*p != '\0' && *p != ';' && n + 1 < sizeof(val))
            {
                val[n++] = *p++;
            }

            val[n] = '\0';

            if (str_equal_flat(val, password))
            {
                return true;
            }
        }
    }

    return false;
}
