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
 * @file event_manager_template.c
 * @brief PURE ${key} template substitution (host-tested): event values
 *        first (+ ts/source/name builtins), then the pull-value
 *        resolver, else the literal `null`. No IDF types.
 */
#include "event_manager_private.h"

#include <stdio.h>
#include <string.h>

void em_kv_render(const em_kv_t *kv, char *out, size_t out_len)
{
    switch (kv->type)
    {
        case EM_VAL_F64:
            snprintf(out, out_len, "%g", kv->v.f64);
            break;
        case EM_VAL_I64:
            snprintf(out, out_len, "%lld", (long long)kv->v.i64);
            break;
        case EM_VAL_BOOL:
            snprintf(out, out_len, "%s", kv->v.b ? "true" : "false");
            break;
        default:
            snprintf(out, out_len, "%s", kv->v.str);
            break;
    }
}

/** Resolve one ${...} name into @p val. */
static void resolve(const char *name, const em_event_t *ev,
                    em_tpl_resolver_t resolver, char *val, size_t val_len)
{
    const em_kv_t *kv = (ev != NULL) ? em_event_get(ev, name) : NULL;

    if (kv != NULL)
    {
        em_kv_render(kv, val, val_len);
        return;
    }

    if (ev != NULL)
    {
        if (strcmp(name, "ts") == 0)
        {
            snprintf(val, val_len, "%lld", (long long)ev->ts_us);
            return;
        }

        if (strcmp(name, "source") == 0)
        {
            snprintf(val, val_len, "%s", ev->source);
            return;
        }

        if (strcmp(name, "name") == 0)
        {
            snprintf(val, val_len, "%s", ev->name);
            return;
        }
    }

    if (resolver != NULL && resolver(name, val, val_len) == ESP_OK)
    {
        return;
    }

    snprintf(val, val_len, "null");
}

esp_err_t em_template_render(const char *tpl, const em_event_t *ev,
                             em_tpl_resolver_t resolver, char *out,
                             size_t out_len)
{
    if (tpl == NULL || out == NULL || out_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t w = 0;

    while (*tpl != '\0')
    {
        if (tpl[0] == '$' && tpl[1] == '{')
        {
            const char *end = strchr(tpl + 2, '}');

            if (end != NULL && (size_t)(end - tpl - 2) < 64)
            {
                char name[64];
                size_t nlen = (size_t)(end - tpl - 2);

                memcpy(name, tpl + 2, nlen);
                name[nlen] = '\0';

                /* pull values can be large (autopid.data snapshots) —
                   render straight into the remaining output space */
                if (w >= out_len - 1)
                {
                    return ESP_ERR_INVALID_SIZE;
                }

                resolve(name, ev, resolver, out + w, out_len - w);
                w += strlen(out + w);
                tpl = end + 1;
                continue;
            }
        }

        if (w >= out_len - 1)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        out[w++] = *tpl++;
    }

    out[w] = '\0';
    return ESP_OK;
}
