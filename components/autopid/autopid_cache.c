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
 * @file autopid_cache.c
 * @brief The live parameter value cache — one slot per pooled parameter,
 *        lock-protected snapshots. The single source every output reads
 *        (API, CLI, ${autopid.data}, Phase-3 events).
 */
#include "autopid_private.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"

typedef struct
{
    double  value;
    int64_t ts_us;
    bool    valid;
} ap_slot_t;

static ap_slot_t s_cache[AP_MAX_PARAMS] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */

/* External injected values (GPS etc.) — small, name-keyed, config-agnostic.
 * Shares s_lock with the slot cache. */
#define AP_MAX_EXTERNAL 12

typedef struct
{
    char    name[AP_NAME_LEN];
    char    unit[AP_UNIT_LEN];
    double  value;
    int64_t ts_us;
    bool    valid;
} ap_ext_slot_t;

static ap_ext_slot_t s_ext[AP_MAX_EXTERNAL] EXT_RAM_BSS_ATTR;

void ap_cache_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    memset(s_cache, 0, sizeof(s_cache));
    memset(s_ext, 0, sizeof(s_ext));
}

void ap_cache_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_cache, 0, sizeof(s_cache));
    /* s_ext is NOT cleared: external samples (GPS) are config-independent
     * — a config reload must not blank the last known position */
    xSemaphoreGive(s_lock);
}

int ap_ext_put(const char *name, const char *unit, double value,
               int64_t ts_us, bool *out_changed)
{
    if (name == NULL || name[0] == '\0')
    {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int slot = -1;
    int free_slot = -1;

    for (int i = 0; i < AP_MAX_EXTERNAL; i++)
    {
        if (s_ext[i].valid && strcmp(s_ext[i].name, name) == 0)
        {
            slot = i;
            break;
        }

        if (free_slot < 0 && !s_ext[i].valid)
        {
            free_slot = i;
        }
    }

    if (slot < 0)
    {
        slot = free_slot;
    }

    if (slot < 0)
    {
        xSemaphoreGive(s_lock); /* table full */
        return -1;
    }

    bool changed = !(s_ext[slot].valid && s_ext[slot].value == value);

    strlcpy(s_ext[slot].name, name, sizeof(s_ext[slot].name));
    strlcpy(s_ext[slot].unit, unit != NULL ? unit : "",
            sizeof(s_ext[slot].unit));
    s_ext[slot].value = value;
    s_ext[slot].ts_us = ts_us;
    s_ext[slot].valid = true;

    xSemaphoreGive(s_lock);

    if (out_changed != NULL)
    {
        *out_changed = changed;
    }

    return slot;
}

bool ap_ext_get(const char *name, double *value, int64_t *ts_us)
{
    if (name == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool found = false;

    for (int i = 0; i < AP_MAX_EXTERNAL; i++)
    {
        if (s_ext[i].valid && strcmp(s_ext[i].name, name) == 0)
        {
            if (value != NULL)
            {
                *value = s_ext[i].value;
            }

            if (ts_us != NULL)
            {
                *ts_us = s_ext[i].ts_us;
            }

            found = true;
            break;
        }
    }

    xSemaphoreGive(s_lock);
    return found;
}

void ap_cache_put(uint16_t slot, double value, int64_t ts_us)
{
    if (slot >= AP_MAX_PARAMS)
    {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cache[slot].value = value;
    s_cache[slot].ts_us = ts_us;
    s_cache[slot].valid = true;
    xSemaphoreGive(s_lock);
}

bool ap_cache_get(uint16_t slot, double *value, int64_t *ts_us)
{
    if (slot >= AP_MAX_PARAMS)
    {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool valid = s_cache[slot].valid;

    if (valid)
    {
        if (value != NULL)
        {
            *value = s_cache[slot].value;
        }

        if (ts_us != NULL)
        {
            *ts_us = s_cache[slot].ts_us;
        }
    }

    xSemaphoreGive(s_lock);
    return valid;
}

/** The LEGACY autopid_data shape: a flat {"Name": value} object of every
 *  parameter holding a valid sample. Caller owns the returned object. */
cJSON *ap_cache_snapshot(const ap_config_t *cfg)
{
    cJSON *out = cJSON_CreateObject();

    if (out == NULL)
    {
        return NULL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (uint16_t i = 0; i < cfg->n_params; i++)
    {
        if (s_cache[i].valid)
        {
            cJSON_AddNumberToObject(out, cfg->params[i].name,
                                    s_cache[i].value);
        }
    }

    /* injected values (GPS etc.) ride the same autopid_data object */
    for (int i = 0; i < AP_MAX_EXTERNAL; i++)
    {
        if (s_ext[i].valid)
        {
            cJSON_AddNumberToObject(out, s_ext[i].name, s_ext[i].value);
        }
    }

    xSemaphoreGive(s_lock);
    return out;
}

/** Rich per-parameter listing for GET /api/autopid / the CLI. */
cJSON *ap_cache_detail(const ap_config_t *cfg)
{
    cJSON *arr = cJSON_CreateArray();

    if (arr == NULL)
    {
        return NULL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (uint16_t i = 0; i < cfg->n_params; i++)
    {
        cJSON *p = cJSON_CreateObject();

        if (p == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(p, "name", cfg->params[i].name);
        cJSON_AddStringToObject(p, "unit", cfg->params[i].unit);

        if (s_cache[i].valid)
        {
            cJSON_AddNumberToObject(p, "value", s_cache[i].value);
            cJSON_AddNumberToObject(p, "ts_us",
                                    (double)s_cache[i].ts_us);
        }
        else
        {
            cJSON_AddNullToObject(p, "value");
        }

        cJSON_AddItemToArray(arr, p);
    }

    /* external injected values (GPS etc.) — listed like real params */
    for (int i = 0; i < AP_MAX_EXTERNAL; i++)
    {
        if (!s_ext[i].valid)
        {
            continue;
        }

        cJSON *p = cJSON_CreateObject();

        if (p == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(p, "name", s_ext[i].name);
        cJSON_AddStringToObject(p, "unit", s_ext[i].unit);
        cJSON_AddNumberToObject(p, "value", s_ext[i].value);
        cJSON_AddNumberToObject(p, "ts_us", (double)s_ext[i].ts_us);
        cJSON_AddBoolToObject(p, "external", true);
        cJSON_AddItemToArray(arr, p);
    }

    xSemaphoreGive(s_lock);
    return arr;
}

int ap_cache_find(const ap_config_t *cfg, const char *param)
{
    for (uint16_t i = 0; i < cfg->n_params; i++)
    {
        if (strcmp(cfg->params[i].name, param) == 0)
        {
            return i;
        }
    }

    return -1;
}
