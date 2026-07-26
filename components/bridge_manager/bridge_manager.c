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
 * @file bridge_manager.c
 * @brief Lifecycle and the endpoint/translator registries. Settings live in
 *        bridge_manager_settings.c (standard §4.1).
 */
#include "bridge_manager.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "log_manager.h"

#include "bridge_manager_private.h"

static const char *TAG = "bridge_manager";

/* registries: static PSRAM tables, write pre-start, read lock-free after */
static bridge_endpoint_t s_eps[BRIDGE_MANAGER_MAX_ENDPOINTS]
    EXT_RAM_BSS_ATTR;
static int s_ep_count;

static bridge_translator_t s_trs[BRIDGE_MANAGER_MAX_TRANSLATORS]
    EXT_RAM_BSS_ATTR;
static int s_tr_count;

static bool s_started;

/* ---- registry lookups ---------------------------------------------------------- */

const void *bm_core_endpoint(const char *name)
{
    for (int i = 0; i < s_ep_count; i++)
    {
        if (strcmp(s_eps[i].name, name) == 0)
        {
            return &s_eps[i];
        }
    }

    return NULL;
}

const void *bm_core_translator(const char *name)
{
    for (int i = 0; i < s_tr_count; i++)
    {
        if (strcmp(s_trs[i].name, name) == 0)
        {
            return &s_trs[i];
        }
    }

    return NULL;
}

int bm_core_endpoint_names(const char **out, bool *multi_out, int cap)
{
    int n = (s_ep_count < cap) ? s_ep_count : cap;

    for (int i = 0; i < n; i++)
    {
        out[i] = s_eps[i].name;

        if (multi_out != NULL)
        {
            multi_out[i] = s_eps[i].multi_consumer;
        }
    }

    return n;
}

int bm_core_translator_names(const char **out, int cap)
{
    int n = (s_tr_count < cap) ? s_tr_count : cap;

    for (int i = 0; i < n; i++)
    {
        out[i] = s_trs[i].name;
    }

    return n;
}

bool bm_core_started(void)
{
    return s_started;
}

void bridge_manager_capacity(int *eps_used, int *eps_cap,
                             int *trs_used, int *trs_cap)
{
    /* health surface: the endpoint table sat at 15/16 unnoticed until
       the 2026-07-19 capacity sweep — the bench asserts headroom now */
    if (eps_used != NULL)
    {
        *eps_used = s_ep_count;
    }

    if (eps_cap != NULL)
    {
        *eps_cap = BRIDGE_MANAGER_MAX_ENDPOINTS;
    }

    if (trs_used != NULL)
    {
        *trs_used = s_tr_count;
    }

    if (trs_cap != NULL)
    {
        *trs_cap = BRIDGE_MANAGER_MAX_TRANSLATORS;
    }
}

/* ---- lifecycle -------------------------------------------------------------------- */

esp_err_t bridge_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "bridge_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return bm_settings_register();
}

esp_err_t bridge_manager_start(void)
{
    if (!bm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    esp_err_t err = bm_pump_start_all();

    if (err == ESP_OK)
    {
        s_started = true;
        ESP_LOGI(TAG, "started (%d bridge slots, %d endpoints, "
                 "%d translators)", bm_settings_count(), s_ep_count,
                 s_tr_count);
    }

    return err;
}

esp_err_t bridge_manager_stop(void)
{
    if (!s_started)
    {
        return ESP_OK;
    }

    bm_pump_stop_all();
    s_started = false;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

/* ---- registration ------------------------------------------------------------------ */

esp_err_t bridge_manager_register_endpoint(const bridge_endpoint_t *ep)
{
    if (ep == NULL || ep->name == NULL || ep->send == NULL ||
        ep->subscribe == NULL || ep->unsubscribe == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_started)
    {
        return ESP_ERR_INVALID_STATE; /* tables are lock-free after start */
    }

    if (s_ep_count >= BRIDGE_MANAGER_MAX_ENDPOINTS ||
        bm_core_endpoint(ep->name) != NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_eps[s_ep_count++] = *ep;
    ESP_LOGI(TAG, "endpoint '%s' registered", ep->name);
    return ESP_OK;
}

esp_err_t bridge_manager_register_translator(const bridge_translator_t *tr)
{
    if (tr == NULL || tr->name == NULL || tr->decode == NULL ||
        tr->encode == NULL || tr->ctx_size > BM_CTX_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_tr_count >= BRIDGE_MANAGER_MAX_TRANSLATORS ||
        bm_core_translator(tr->name) != NULL ||
        strcmp(tr->name, "raw") == 0)
    {
        return ESP_ERR_NO_MEM; /* full, duplicate, or the built-in name */
    }

    s_trs[s_tr_count++] = *tr;
    ESP_LOGI(TAG, "translator '%s' registered (ctx %u B)", tr->name,
             (unsigned)tr->ctx_size);
    return ESP_OK;
}

esp_err_t bridge_manager_stats(const char *bridge_name, bridge_stats_t *out)
{
    if (bridge_name == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return bm_pump_stats(bridge_name, out);
}
