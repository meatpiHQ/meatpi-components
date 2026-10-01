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
 * @file autopid_vehicle_index.c
 * @brief PURE vehicle store index (TASK_quick_setup.md, second pass):
 *        key derivation, find by key / VIN / fingerprint, the fingerprint
 *        SUBSET rule, the LRU eviction pick and the change-guarded
 *        last_seen touch. The JSON codec is autopid_vehicle_codec.c. No
 *        IDF dependencies; host-tested
 *        (host_test/main/test_vehicle_index.c).
 */
#include "autopid_private.h"

#include <stdio.h>
#include <string.h>

static bool has(const char *s)
{
    return s != NULL && s[0] != '\0';
}

static bool fp_valid(const char *fp)
{
    if (fp == NULL || strlen(fp) != AP_FP_LEN - 1)
    {
        return false;
    }

    for (int i = 0; i < AP_FP_LEN - 1; i++)
    {
        char c = fp[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }

    return true;
}

/* ---- keys + names ------------------------------------------------------------- */

void ap_vidx_init(ap_veh_index_t *idx)
{
    if (idx != NULL)
    {
        memset(idx, 0, sizeof(*idx));
        idx->current = -1;
    }
}

void ap_vidx_key_for(const char *vin, const char *fp,
                     char out[AP_VEH_KEY_LEN])
{
    if (out == NULL)
    {
        return;
    }

    out[0] = '\0';

    if (ap_veh_vin_valid(vin))
    {
        snprintf(out, AP_VEH_KEY_LEN, "%s", vin);
    }
    else if (fp_valid(fp))
    {
        snprintf(out, AP_VEH_KEY_LEN, "fp:%s", fp);
    }
}

bool ap_vidx_key_valid(const char *key)
{
    if (key == NULL)
    {
        return false;
    }

    if (strncmp(key, "fp:", 3) == 0)
    {
        return fp_valid(key + 3);
    }

    return ap_veh_vin_valid(key);
}

void ap_vidx_default_name(const char *vin, const char *fp,
                          char out[AP_VEH_NAME_LEN])
{
    if (out == NULL)
    {
        return;
    }

    if (ap_veh_vin_valid(vin))
    {
        /* the WMI (make code) + the serial tail: "1WC 0001" */
        snprintf(out, AP_VEH_NAME_LEN, "%.3s %s", vin, vin + 13);
    }
    else if (fp_valid(fp))
    {
        snprintf(out, AP_VEH_NAME_LEN, "Car %.4s", fp);
    }
    else
    {
        snprintf(out, AP_VEH_NAME_LEN, "Vehicle");
    }
}

/* ---- lookups -------------------------------------------------------------------- */

int ap_vidx_find_key(const ap_veh_index_t *idx, const char *key)
{
    if (idx == NULL || !has(key))
    {
        return -1;
    }

    for (int i = 0; i < idx->n; i++)
    {
        if (strcmp(idx->v[i].key, key) == 0)
        {
            return i;
        }
    }

    return -1;
}

int ap_vidx_find_vin(const ap_veh_index_t *idx, const char *vin)
{
    if (idx == NULL || !has(vin))
    {
        return -1;
    }

    for (int i = 0; i < idx->n; i++)
    {
        if (strcmp(idx->v[i].vin, vin) == 0)
        {
            return i;
        }
    }

    return -1;
}

int ap_vidx_find_fp(const ap_veh_index_t *idx, const char *fp)
{
    if (idx == NULL || !has(fp))
    {
        return -1;
    }

    for (int i = 0; i < idx->n; i++)
    {
        if (strcmp(idx->v[i].fingerprint, fp) == 0)
        {
            return i;
        }
    }

    return -1;
}

/* ---- the subset rule -------------------------------------------------------------- */

static const ap_veh_ecu_t *main_ecu(const ap_veh_ecu_t *set, size_t n)
{
    const ap_veh_ecu_t *m = &set[0];

    for (size_t i = 1; i < n; i++)
    {
        if (set[i].id < m->id)
        {
            m = &set[i];
        }
    }

    return m;
}

static bool ecu_in(const ap_veh_ecu_t *e, const ap_veh_ecu_t *set, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (set[i].id == e->id)
        {
            return set[i].bitmap == e->bitmap;
        }
    }

    return false;
}

bool ap_veh_ecus_subset(const ap_veh_ecu_t *a, size_t na,
                        const ap_veh_ecu_t *b, size_t nb)
{
    if (a == NULL || b == NULL || na == 0 || nb == 0)
    {
        return false;
    }

    /* the main ECU (the lowest id: 7E8 / 18DAF110, the engine or the
       gateway) must be present on both sides with the same bitmap */
    const ap_veh_ecu_t *ma = main_ecu(a, na);
    const ap_veh_ecu_t *mb = main_ecu(b, nb);

    if (ma->id != mb->id || ma->bitmap != mb->bitmap)
    {
        return false;
    }

    /* every responder of the smaller set appears in the larger one */
    const ap_veh_ecu_t *small = (na <= nb) ? a : b;
    const ap_veh_ecu_t *large = (na <= nb) ? b : a;
    size_t ns = (na <= nb) ? na : nb;
    size_t nl = (na <= nb) ? nb : na;

    for (size_t i = 0; i < ns; i++)
    {
        if (!ecu_in(&small[i], large, nl))
        {
            return false;
        }
    }

    return true;
}

int ap_vidx_match(const ap_veh_index_t *idx, const char *vin,
                  const ap_veh_ecu_t *ecus, size_t n_ecus, const char *fp,
                  bool *exact)
{
    bool ex = true;
    int found = -1;

    if (idx != NULL && has(vin))
    {
        found = ap_vidx_find_vin(idx, vin);

        /* a car first learned without its VIN (a fp: key) whose VIN now
           answers: adopt the entry rather than duplicate the car */
        for (int i = 0; found < 0 && i < idx->n; i++)
        {
            const ap_veh_entry_t *e = &idx->v[i];

            if (e->vin[0] != '\0')
            {
                continue;
            }

            if (has(fp) && strcmp(e->fingerprint, fp) == 0)
            {
                found = i;
            }
            else if (ap_veh_ecus_subset(e->ecus, e->n_ecus, ecus, n_ecus))
            {
                found = i;
                ex = false;
            }
        }
    }
    else if (idx != NULL)
    {
        found = ap_vidx_find_fp(idx, fp);

        for (int i = 0; found < 0 && i < idx->n; i++)
        {
            const ap_veh_entry_t *e = &idx->v[i];

            if (ap_veh_ecus_subset(e->ecus, e->n_ecus, ecus, n_ecus))
            {
                found = i;
                ex = false;
            }
        }
    }

    if (exact != NULL)
    {
        *exact = (found >= 0) ? ex : true;
    }

    return found;
}

/* ---- eviction + mutation ----------------------------------------------------------- */

int ap_vidx_lru(const ap_veh_index_t *idx)
{
    int pick = -1;

    if (idx == NULL)
    {
        return -1;
    }

    for (int i = 0; i < idx->n; i++)
    {
        if (i == idx->current)
        {
            continue;           /* the connected car never goes         */
        }

        if (pick < 0)
        {
            pick = i;
            continue;
        }

        const ap_veh_entry_t *e = &idx->v[i];
        const ap_veh_entry_t *p = &idx->v[pick];

        if (e->last_seen < p->last_seen ||
            (e->last_seen == p->last_seen && e->first_seen < p->first_seen))
        {
            pick = i;
        }
    }

    return pick;
}

void ap_vidx_remove(ap_veh_index_t *idx, int i)
{
    if (idx == NULL || i < 0 || i >= idx->n)
    {
        return;
    }

    for (int k = i; k + 1 < idx->n; k++)
    {
        idx->v[k] = idx->v[k + 1];
    }

    idx->n--;
    memset(&idx->v[idx->n], 0, sizeof(idx->v[idx->n]));

    if (idx->current == i)
    {
        idx->current = -1;
    }
    else if (idx->current > i)
    {
        idx->current--;
    }
}

int ap_vidx_add(ap_veh_index_t *idx, const ap_veh_entry_t *e,
                ap_veh_entry_t *evicted)
{
    if (evicted != NULL)
    {
        memset(evicted, 0, sizeof(*evicted));
    }

    if (idx == NULL || e == NULL || !has(e->key) ||
        ap_vidx_find_key(idx, e->key) >= 0)
    {
        return -1;
    }

    if (idx->n >= AP_VEH_MAX)
    {
        int l = ap_vidx_lru(idx);

        if (l < 0)
        {
            return -1;
        }

        if (evicted != NULL)
        {
            *evicted = idx->v[l];
        }

        ap_vidx_remove(idx, l);
    }

    idx->v[idx->n] = *e;
    return idx->n++;
}

bool ap_vidx_touch(ap_veh_entry_t *e, int64_t now)
{
    bool changed = false;

    if (e == NULL || now <= 0)
    {
        return false;           /* clock unset: never write for a touch */
    }

    if (e->first_seen == 0)
    {
        e->first_seen = now;
        changed = true;
    }

    if (e->last_seen == 0 || now - e->last_seen >= AP_VEH_TOUCH_PERIOD_S)
    {
        e->last_seen = now;
        changed = true;
    }

    return changed;
}

