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
 * @file usb_acm_nmea.c
 * @brief NMEA 0183 sentences into the shared GPS fix (see the header).
 *        Only RMC and GGA are read: RMC carries the fix flag, position,
 *        speed and course; GGA the satellites, HDOP and altitude.
 */
#include "usb_acm_nmea.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define NMEA_MAX_FIELDS 20

void usb_acm_nmea_init(usb_acm_nmea_t *st)
{
    if (st != NULL)
    {
        memset(st, 0, sizeof(*st));
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}

/* "$<body>*hh": the XOR of the body against the two hex digits */
static bool checksum_ok(const char *line, const char **star_out)
{
    const char *star = strchr(line, '*');

    if (line[0] != '$' || star == NULL || star == line + 1)
    {
        return false;
    }

    int hi = hex_val(star[1]);
    int lo = star[1] != '\0' ? hex_val(star[2]) : -1;

    if (hi < 0 || lo < 0)
    {
        return false;
    }

    uint8_t sum = 0;

    for (const char *p = line + 1; p < star; p++)
    {
        sum ^= (uint8_t)*p;
    }

    if (star_out != NULL)
    {
        *star_out = star;
    }
    return sum == (uint8_t)((hi << 4) | lo);
}

bool usb_acm_nmea_is_sentence(const char *line)
{
    if (line == NULL || strlen(line) < 10 || line[0] != '$' || line[6] != ',')
    {
        return false;
    }
    for (int i = 1; i <= 5; i++)
    {
        if (!isupper((unsigned char)line[i]))
        {
            return false;
        }
    }
    return checksum_ok(line, NULL);
}

/* split the body (between `$` and `*`) on commas; empty fields stay empty */
static int split(const char *line, char *buf, size_t buf_len,
                 const char **fields, int max_fields)
{
    const char *star = strchr(line, '*');
    size_t n = star != NULL ? (size_t)(star - line) : strlen(line);

    if (n >= buf_len)
    {
        n = buf_len - 1;
    }
    memcpy(buf, line, n);
    buf[n] = '\0';

    int count = 0;
    char *p = buf;

    fields[count++] = p;
    while (*p != '\0' && count < max_fields)
    {
        if (*p == ',')
        {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

/* ddmm.mmmm (or dddmm.mmmm) + N/S/E/W into signed decimal degrees */
static bool coord(const char *v, const char *hemi, double *out)
{
    if (v[0] == '\0' || hemi[0] == '\0')
    {
        return false;
    }

    double raw = atof(v);
    double deg = (double)(long)(raw / 100.0);   /* raw is never negative */
    double min = raw - deg * 100.0;
    double d = deg + min / 60.0;

    if (hemi[0] == 'S' || hemi[0] == 'W')
    {
        d = -d;
    }
    *out = d;
    return true;
}

int usb_acm_nmea_feed_line(usb_acm_nmea_t *st, const char *line,
                           usb_acm_gps_t *out)
{
    if (st == NULL || line == NULL)
    {
        return 0;
    }
    if (line[0] != '$')
    {
        return 0;
    }
    if (!checksum_ok(line, NULL))
    {
        st->bad_checksum++;
        return 0;
    }
    if (!usb_acm_nmea_is_sentence(line))
    {
        return 0;
    }
    st->sentences++;

    char buf[USB_ACM_NMEA_LINE_MAX + 1];
    const char *f[NMEA_MAX_FIELDS] = { 0 };
    int n = split(line, buf, sizeof(buf), f, NMEA_MAX_FIELDS);
    const char *fmt = f[0] + 3;   /* past "$GP" */

    if (strcmp(fmt, "GGA") == 0 && n >= 10)
    {
        st->have_gga = true;
        st->quality = atoi(f[6]);
        st->satellites = atoi(f[7]);
        st->hdop = f[8][0] != '\0' ? atof(f[8]) : 0.0;
        st->altitude_m = f[9][0] != '\0' ? atof(f[9]) : 0.0;
        return 0;
    }

    if (strcmp(fmt, "RMC") == 0 && n >= 9)
    {
        if (f[2][0] != 'A')
        {
            return -1;
        }
        if (out == NULL)
        {
            return 1;
        }

        usb_acm_gps_t g;

        memset(&g, 0, sizeof(g));
        if (!coord(f[3], f[4], &g.latitude) || !coord(f[5], f[6], &g.longitude))
        {
            return -1;
        }
        g.valid = true;
        g.speed_kmph = f[7][0] != '\0' ? atof(f[7]) * 1.852 : 0.0;
        g.heading_deg = f[8][0] != '\0' ? atof(f[8]) : 0.0;
        if (st->have_gga)
        {
            g.satellites = st->satellites;
            g.altitude_m = st->altitude_m;
            g.accuracy_m = (int)(st->hdop * 5.0 + 0.5);   /* HDOP x 5, rounded */
        }
        *out = g;
        return 1;
    }

    return 0;
}

int usb_acm_nmea_feed_bytes(usb_acm_nmea_t *st, const uint8_t *data,
                            size_t len, usb_acm_gps_t *out)
{
    int result = 0;

    if (st == NULL || data == NULL)
    {
        return 0;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = (char)data[i];

        if (c == '\r' || c == '\n')
        {
            if (st->line_len > 0 && !st->overflow)
            {
                st->line[st->line_len] = '\0';
                if (st->line[0] == '$')
                {
                    int r = usb_acm_nmea_feed_line(st, st->line, out);

                    if (r != 0)
                    {
                        result = r;
                    }
                }
            }
            st->line_len = 0;
            st->overflow = false;
            continue;
        }
        if (c == '$')
        {
            /* a sentence start always begins a fresh line */
            st->line_len = 0;
            st->overflow = false;
        }
        if (st->line_len < USB_ACM_NMEA_LINE_MAX)
        {
            st->line[st->line_len++] = c;
        }
        else
        {
            st->overflow = true;
        }
    }
    return result;
}
