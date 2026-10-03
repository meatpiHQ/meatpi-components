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
 * @file j1939_read.c
 * @brief The readers' side of the J1939 listener: the newest message of a
 *        group, a decoded value, the VIN, a controller's active trouble
 *        codes, the walk over everything stored, the sources.
 *
 * Every function takes the listener's lock for a lookup and a copy and
 * nothing else, so a reader never holds up the receive task for longer than
 * a memcpy of one message (1785 bytes at most).
 */
#include "j1939.h"

#include <string.h>

#include "esp_timer.h"

#include "j1939_private.h"

static uint32_t us_to_ms(int64_t us)
{
    if (us <= 0)
    {
        return 0;
    }

    int64_t ms = us / 1000;

    return (ms > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
}

/** Describe @p e and copy its payload. Called with the lock held. */
static void entry_out(const j1939_cache_t *c, const j1939_entry_t *e,
                      int64_t now_us, j1939_msg_t *info, uint8_t *buf,
                      size_t cap)
{
    if (info != NULL)
    {
        info->pgn = e->pgn;
        info->sa = e->sa;
        info->da = e->da;
        info->len = e->len;
        info->count = e->count;
        info->period_ms = e->period_us / 1000u;
        info->age_ms = us_to_ms(now_us - e->rx_us);
    }

    if (buf != NULL && cap > 0 && e->len > 0)
    {
        memcpy(buf, j1939_cache_payload(c, e), (e->len < cap) ? e->len : cap);
    }
}

esp_err_t j1939_pgn_latest(uint32_t pgn, int sa, int da, j1939_msg_t *info,
                           uint8_t *buf, size_t cap)
{
    int64_t now = esp_timer_get_time();
    esp_err_t rc = ESP_ERR_NOT_FOUND;

    j1939_lock();

    const j1939_cache_t *c = j1939_priv_cache();
    const j1939_entry_t *e = j1939_cache_get(c, pgn, sa, da, now);

    if (e != NULL)
    {
        entry_out(c, e, now, info, buf, cap);
        rc = ESP_OK;
    }

    j1939_unlock();
    return rc;
}

esp_err_t j1939_spn_latest(const j1939_spn_t *spn, int sa, double *value,
                           j1939_raw_t *raw, j1939_msg_t *info)
{
    int64_t now = esp_timer_get_time();
    esp_err_t rc = ESP_ERR_NOT_FOUND;

    if (spn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    j1939_lock();

    const j1939_cache_t *c = j1939_priv_cache();
    const j1939_entry_t *e = j1939_cache_get(c, spn->pgn, sa, J1939_ADDR_ANY,
                                             now);

    if (e != NULL)
    {
        /* decoded in place: a value may sit anywhere in a long message */
        j1939_raw_t r = j1939_spn_decode(spn, j1939_cache_payload(c, e),
                                         e->len, value);

        if (raw != NULL)
        {
            *raw = r;
        }

        entry_out(c, e, now, info, NULL, 0);
        rc = ESP_OK;
    }

    j1939_unlock();
    return rc;
}

bool j1939_vin(char out[J1939_VIN_LEN + 1], uint8_t *sa)
{
    int64_t now = esp_timer_get_time();
    bool ok = false;

    j1939_lock();

    const j1939_cache_t *c = j1939_priv_cache();
    const j1939_entry_t *e = j1939_cache_get(c, J1939_PGN_VIN, J1939_ADDR_ANY,
                                             J1939_ADDR_ANY, now);

    if (e != NULL)
    {
        ok = j1939_vin_parse(j1939_cache_payload(c, e), e->len, out);

        if (ok && sa != NULL)
        {
            *sa = e->sa;
        }
    }

    j1939_unlock();
    return ok;
}

/** DM1 and DM2 share one layout (j1939_dm_parse): the lamps, then the
 *  codes. @p pgn says which group. */
static esp_err_t dm_read(uint32_t pgn, uint8_t sa, j1939_lamps_t *lamps,
                         j1939_dtc_t *out, size_t max, size_t *count,
                         j1939_msg_t *info)
{
    int64_t now = esp_timer_get_time();
    esp_err_t rc = ESP_ERR_NOT_FOUND;

    j1939_lock();

    const j1939_cache_t *c = j1939_priv_cache();
    /* broadcast as a rule (DM1), or answered to a request, ours or
       somebody's (DM2, and a DM1 that was asked for) */
    const j1939_entry_t *e = j1939_cache_get(c, pgn, sa, J1939_ADDR_ANY, now);

    if (e != NULL)
    {
        size_t n = j1939_dm_parse(j1939_cache_payload(c, e), e->len, lamps,
                                  out, max);

        if (count != NULL)
        {
            *count = n;
        }

        entry_out(c, e, now, info, NULL, 0);
        rc = ESP_OK;
    }

    j1939_unlock();
    return rc;
}

esp_err_t j1939_dm1(uint8_t sa, j1939_lamps_t *lamps, j1939_dtc_t *out,
                    size_t max, size_t *count, j1939_msg_t *info)
{
    return dm_read(J1939_PGN_DM1, sa, lamps, out, max, count, info);
}

esp_err_t j1939_dm2(uint8_t sa, j1939_lamps_t *lamps, j1939_dtc_t *out,
                    size_t max, size_t *count, j1939_msg_t *info)
{
    return dm_read(J1939_PGN_DM2, sa, lamps, out, max, count, info);
}

bool j1939_msg_next(size_t *cursor, j1939_msg_t *info, uint8_t *buf,
                    size_t cap)
{
    int64_t now = esp_timer_get_time();
    bool found = false;

    if (cursor == NULL)
    {
        return false;
    }

    j1939_lock();

    const j1939_cache_t *c = j1939_priv_cache();

    while (*cursor < J1939_CACHE_SLOTS && !found)
    {
        const j1939_entry_t *e = j1939_cache_at(c, *cursor);

        (*cursor)++;

        if (e != NULL)
        {
            entry_out(c, e, now, info, buf, cap);
            found = true;
        }
    }

    j1939_unlock();
    return found;
}

/** Describe source @p sa. Called with the lock held. False: never heard. */
static bool source_out(const j1939_sources_t *s, uint8_t sa, int64_t now_us,
                       j1939_source_t *out)
{
    if (s->frames[sa] == 0)
    {
        return false;
    }

    out->sa = sa;
    out->frames = s->frames[sa];
    out->age_ms = us_to_ms(now_us - s->last_us[sa]);
    out->named = j1939_sources_name(s, sa, out->name);

    if (!out->named)
    {
        memset(out->name, 0, sizeof(out->name));
    }

    return true;
}

size_t j1939_sources(j1939_source_t *out, size_t max)
{
    int64_t now = esp_timer_get_time();
    size_t n = 0;

    if (out == NULL)
    {
        return 0;
    }

    j1939_lock();

    for (unsigned sa = 0; sa < 256 && n < max; sa++)
    {
        if (source_out(j1939_priv_sources(), (uint8_t)sa, now, &out[n]))
        {
            n++;
        }
    }

    j1939_unlock();
    return n;
}

bool j1939_source(uint8_t sa, j1939_source_t *out)
{
    int64_t now = esp_timer_get_time();
    bool heard;

    if (out == NULL)
    {
        return false;
    }

    j1939_lock();
    heard = source_out(j1939_priv_sources(), sa, now, out);
    j1939_unlock();
    return heard;
}
