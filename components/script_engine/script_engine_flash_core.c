/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_flash_core.c
 * @brief The flash lock's rules and the checksum helpers (pure).
 */
#include "script_engine_flash_core.h"

#include <string.h>

const char *se_lock_state_str(se_lock_state_t s)
{
    switch (s)
    {
        case SE_LOCK_IDLE:      return "idle";
        case SE_LOCK_COMMITTED: return "committed";
        case SE_LOCK_FAILED:    return "failed";
        case SE_LOCK_RECOVERY:  return "recovery";
        default:                return "?";
    }
}

bool se_lock_held(se_lock_state_t s)
{
    return s == SE_LOCK_COMMITTED || s == SE_LOCK_FAILED ||
           s == SE_LOCK_RECOVERY;
}

se_lock_state_t se_lock_on_commit(se_lock_state_t s)
{
    (void)s;
    return SE_LOCK_COMMITTED;
}

se_lock_state_t se_lock_on_run_end(se_lock_state_t s, bool ok, bool aborted)
{
    if (s != SE_LOCK_COMMITTED)
    {
        return s; /* a run that never committed leaves the lock as it was */
    }

    return (ok && !aborted) ? SE_LOCK_IDLE : SE_LOCK_FAILED;
}

se_lock_state_t se_lock_on_release(se_lock_state_t s)
{
    return (s == SE_LOCK_FAILED || s == SE_LOCK_RECOVERY) ? SE_LOCK_IDLE : s;
}

se_lock_state_t se_lock_on_boot(bool committed_record)
{
    return committed_record ? SE_LOCK_RECOVERY : SE_LOCK_IDLE;
}

bool se_lock_tx_allowed(se_lock_state_t s, bool from_runner)
{
    return !se_lock_held(s) || from_runner;
}

/* The path of a URI without its query. */
static bool path_is(const char *uri, const char *path)
{
    size_t n = strlen(path);

    return strncmp(uri, path, n) == 0 && (uri[n] == '\0' || uri[n] == '?');
}

bool se_lock_http_allowed(se_lock_state_t s, bool read_only,
                          const char *uri, bool run_active)
{
    if (!se_lock_held(s) || read_only)
    {
        return true;
    }

    if (uri == NULL)
    {
        return false;
    }

    /* the run's own controls: stop (an abort request once committed),
       answering its prompt, and leaving a failed lock */
    if (path_is(uri, "/api/scripts/stop") ||
        path_is(uri, "/api/scripts/answer") ||
        path_is(uri, "/api/scripts/release"))
    {
        return true;
    }

    /* a recovery script, only while nothing runs and only from a held
       lock that no run owns (failed or recovery) */
    if (path_is(uri, "/api/scripts/run") || path_is(uri, "/api/scripts/check"))
    {
        return s != SE_LOCK_COMMITTED && !run_active;
    }

    return false;
}

/* ---- checksums ------------------------------------------------------------- */

static uint32_t reflect(uint32_t v, uint8_t width)
{
    uint32_t r = 0;

    for (uint8_t i = 0; i < width; i++)
    {
        r = (r << 1) | (v & 1U);
        v >>= 1;
    }

    return r;
}

bool se_crc(const se_crc_spec_t *spec, const uint8_t *d, size_t n,
            uint32_t *out)
{
    if (spec == NULL || spec->width == 0 || spec->width > 32 || out == NULL)
    {
        return false;
    }

    uint8_t w = spec->width;
    uint32_t mask = (w == 32) ? 0xFFFFFFFFU : ((1U << w) - 1U);
    uint32_t top = 1U << (w - 1);
    uint32_t crc = spec->init & mask;

    for (size_t i = 0; i < n; i++)
    {
        uint8_t byte = spec->refin ? (uint8_t)reflect(d[i], 8) : d[i];

        for (int b = 7; b >= 0; b--)
        {
            bool in = ((byte >> b) & 1U) != 0;
            bool msb = (crc & top) != 0;

            crc = (crc << 1) & mask;

            if (in != msb)
            {
                crc ^= spec->poly & mask;
            }
        }
    }

    if (spec->refout)
    {
        crc = reflect(crc, w);
    }

    *out = (crc ^ spec->xorout) & mask;
    return true;
}

uint32_t se_adler32(uint32_t adler, const uint8_t *d, size_t n)
{
    uint32_t a = adler & 0xFFFFU;
    uint32_t b = (adler >> 16) & 0xFFFFU;

    for (size_t i = 0; i < n; i++)
    {
        a = (a + d[i]) % 65521U;
        b = (b + a) % 65521U;
    }

    return (b << 16) | a;
}

uint32_t se_word_sum(const uint8_t *d, size_t n, uint8_t width,
                     bool big_endian)
{
    if (width != 1 && width != 2 && width != 4)
    {
        return 0;
    }

    uint32_t sum = 0;

    for (size_t i = 0; i < n; i += width)
    {
        uint32_t word = 0;

        for (uint8_t k = 0; k < width; k++)
        {
            uint8_t byte = (i + k < n) ? d[i + k] : 0;
            uint8_t shift = big_endian ? (uint8_t)(8 * (width - 1 - k))
                                       : (uint8_t)(8 * k);

            word |= (uint32_t)byte << shift;
        }

        sum += word;
    }

    return sum;
}
