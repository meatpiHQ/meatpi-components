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
 * @file can_core_filter.c
 * @brief CAN frame filter, mask utilities, and hex formatting helpers.
 */

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "can_core_filter.h"

/* -------------------------------------------------------------------------
 * can_core_filter_match
 * ------------------------------------------------------------------------- */
bool can_core_filter_match(uint32_t id, uint32_t filter, uint32_t mask)
{
    return (id & mask) == (filter & mask);
}

/* -------------------------------------------------------------------------
 * can_core_format_id_11
 * ------------------------------------------------------------------------- */
void can_core_format_id_11(uint32_t id, char *buf)
{
    snprintf(buf, 4, "%03X", (unsigned)(id & 0x7FFu));
}

/* -------------------------------------------------------------------------
 * can_core_format_id_29
 * ------------------------------------------------------------------------- */
void can_core_format_id_29(uint32_t id, char *buf)
{
    snprintf(buf, 9, "%08X", (unsigned)(id & 0x1FFFFFFFu));
}

/* -------------------------------------------------------------------------
 * can_core_parse_id
 *
 * Accepts:
 *   3 hex digits  → 11-bit ID  (ext = false)
 *   8 hex digits  → 29-bit ID  (ext = true)
 * Any other length is an error.
 * ------------------------------------------------------------------------- */
bool can_core_parse_id(const char *str, uint32_t *out_id, bool *out_ext)
{
    if (!str || !out_id || !out_ext)
    {
        return false;
    }

    size_t len = strlen(str);

    /* Validate that all characters are hex digits */
    for (size_t i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)str[i]))
        {
            return false;
        }
    }

    if (len == 3)
    {
        *out_id  = (uint32_t)strtoul(str, NULL, 16) & 0x7FFu;
        *out_ext = false;
        return true;
    }

    if (len == 8)
    {
        *out_id  = (uint32_t)strtoul(str, NULL, 16) & 0x1FFFFFFFu;
        *out_ext = true;
        return true;
    }

    /* Also accept 1–7 digit inputs with leading zeros implied for flexibility */
    if (len >= 1 && len <= 7)
    {
        uint32_t val = (uint32_t)strtoul(str, NULL, 16);
        if (len <= 3)
        {
            *out_id  = val & 0x7FFu;
            *out_ext = false;
        }
        else
        {
            *out_id  = val & 0x1FFFFFFFu;
            *out_ext = true;
        }
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------
 * can_core_parse_byte
 * ------------------------------------------------------------------------- */
bool can_core_parse_byte(const char *str, uint8_t *out_byte)
{
    if (!str || !out_byte)
    {
        return false;
    }

    size_t len = strlen(str);
    if (len == 0 || len > 2)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)str[i]))
        {
            return false;
        }
    }

    *out_byte = (uint8_t)strtoul(str, NULL, 16);
    return true;
}

/* -------------------------------------------------------------------------
 * can_core_parse_bytes
 * ------------------------------------------------------------------------- */
bool can_core_parse_bytes(const char *str,
                             uint8_t *out_buf,
                             size_t max_len,
                             size_t *out_len)
{
    if (!str || !out_buf || !out_len)
    {
        return false;
    }

    *out_len = 0;
    const char *p = str;

    while (*p && *out_len < max_len)
    {
        /* Skip whitespace */
        while (*p == ' ')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }

        /* Collect up to 2 hex digits */
        char hex[3] = { 0, 0, 0 };
        if (!isxdigit((unsigned char)p[0]))
        {
            return false;
        }
        hex[0] = p[0];
        p++;

        if (isxdigit((unsigned char)p[0]))
        {
            hex[1] = p[0];
            p++;
        }

        out_buf[(*out_len)++] = (uint8_t)strtoul(hex, NULL, 16);
    }

    return (*out_len > 0);
}

/* -------------------------------------------------------------------------
 * can_core_format_bytes
 * ------------------------------------------------------------------------- */
void can_core_format_bytes(const uint8_t *data,
                              size_t len,
                              char *buf,
                              size_t buf_sz,
                              bool spaces)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < buf_sz; i++)
    {
        if (spaces && i > 0 && pos + 1 < buf_sz)
        {
            buf[pos++] = ' ';
        }
        snprintf(buf + pos, buf_sz - pos, "%02X", (unsigned)data[i]);
        pos += 2;
    }
    if (pos < buf_sz)
    {
        buf[pos] = '\0';
    }
}
