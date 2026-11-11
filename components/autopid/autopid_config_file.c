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
 * @file autopid_config_file.c
 * @brief The PID/filter tables' file, /data/autopid/config.json (target
 *        only): load with the repairs of what an earlier build stored,
 *        save, and the PSRAM copy of the raw text for the readers that
 *        must not touch flash. The parser is autopid_config.c (pure).
 *
 * Split out of autopid_config.c on 2026-11-11 (the 700-line rule), nothing
 * else changed by the move.
 */
#include "autopid_private.h"

#include "autopid_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "filesystem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "autopid";

#define AP_CONFIG_PATH "/data/autopid/config.json"
#define AP_CONFIG_MAX  (256 * 1024)

/* PSRAM cache of the last-loaded raw config JSON, so a PSRAM-stack
 * consumer (ha_webhooks poster) can read the config section WITHOUT
 * touching flash (§2 corollary). Populated on every load, read via
 * autopid_config_json_dup(). Its own lock: independent of autopid's
 * table lock in autopid.c. */
static char             *s_raw_json;
static SemaphoreHandle_t s_raw_lock;

static void raw_cache_store(const char *json, size_t len)
{
    if (s_raw_lock == NULL)
    {
        s_raw_lock = xSemaphoreCreateMutex(); /* boot single-threaded */
    }

    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT);

    if (copy == NULL)
    {
        return; /* keep the previous cache; the section just goes stale */
    }

    memcpy(copy, json, len);
    copy[len] = '\0';

    if (s_raw_lock != NULL)
    {
        xSemaphoreTake(s_raw_lock, portMAX_DELAY);
    }

    free(s_raw_json);
    s_raw_json = copy;

    if (s_raw_lock != NULL)
    {
        xSemaphoreGive(s_raw_lock);
    }
}

esp_err_t autopid_config_json_dup(char **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = NULL;

    if (s_raw_lock == NULL || s_raw_json == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* no config loaded yet */
    }

    xSemaphoreTake(s_raw_lock, portMAX_DELAY);

    size_t len = strlen(s_raw_json);
    char  *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_8BIT);

    if (copy != NULL)
    {
        memcpy(copy, s_raw_json, len + 1);
    }

    xSemaphoreGive(s_raw_lock);

    if (copy == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    *out = copy;
    return ESP_OK;
}

esp_err_t autopid_config_load(ap_config_t *cfg)
{
    size_t size = 0;

    if (filesystem_size(AP_CONFIG_PATH, &size) != ESP_OK || size == 0)
    {
        ESP_LOGW(TAG, "no config file (%s): empty tables",
                 AP_CONFIG_PATH);
        memset(cfg, 0, sizeof(*cfg));
        return ESP_ERR_NOT_FOUND;
    }

    if (size > AP_CONFIG_MAX)
    {
        ESP_LOGE(TAG, "config file too large (%u)", (unsigned)size);
        memset(cfg, 0, sizeof(*cfg));
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = heap_caps_malloc(size + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    esp_err_t err = filesystem_read(AP_CONFIG_PATH, buf, size, &got);

    if (err != ESP_OK)
    {
        free(buf);
        memset(cfg, 0, sizeof(*cfg));
        return err;
    }

    buf[got] = '\0';

    /* a file with a parameter name twice (the SAE table used to give one
       name to two PIDs, and the detection stored its rows unvalidated,
       2026-10-06) is repaired, not emptied: the later name gets a suffix,
       the file is rewritten once, so the tables load, the boot stays
       clean and the UI's PUT of what it reads back goes through */
    int renamed = 0;
    char *fixed = ap_config_repair_names(buf, &renamed);

    if (fixed != NULL)
    {
        ESP_LOGW(TAG, "config file: %d repeated parameter name%s made unique "
                      "(_2, _3, ...) and the file rewritten",
                 renamed, renamed == 1 ? "" : "s");
        free(buf);
        buf = fixed;
        got = strlen(buf);
        (void)filesystem_write(AP_CONFIG_PATH, buf, got);
    }

    /* the standard rows stored by a build before 2026-10-24 count the
       framing-free payload; the expressions count the frame as the chip
       prints it since (issue #1014), and a car the store knows keeps its
       rows across detections: the table's current expressions, rewritten
       once (change-guarded: a file already up to date writes nothing) */
    int renumbered = 0;

    fixed = ap_config_refresh_std(buf, &renumbered);

    if (fixed != NULL)
    {
        ESP_LOGW(TAG, "config file: %d standard-row expression%s re-numbered "
                      "to the frame as the chip prints it and the file "
                      "rewritten",
                 renumbered, renumbered == 1 ? "" : "s");
        free(buf);
        buf = fixed;
        got = strlen(buf);
        (void)filesystem_write(AP_CONFIG_PATH, buf, got);
    }

    char perr[96] = "";

    err = ap_config_parse(buf, cfg, perr, sizeof(perr));

    if (err == ESP_OK)
    {
        raw_cache_store(buf, got); /* PSRAM cache for the HA poster */
    }

    free(buf);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "config file invalid: %s, empty tables", perr);
    }
    else
    {
        ESP_LOGI(TAG, "config: %u groups, %u pids, %u filters, %u params",
                 cfg->n_groups, cfg->n_pids, cfg->n_filters,
                 cfg->n_params);
    }

    return err;
}

esp_err_t autopid_config_save(const char *json, size_t len)
{
    /* filesystem_write is atomic (temp + rename) */
    esp_err_t err = filesystem_write(AP_CONFIG_PATH, json, len);

    if (err == ESP_OK)
    {
        /* the live tables belong to the current car: keep its file in
           step (vehicle store; change-guarded, internal-stack caller) */
        ap_vehicle_config_saved(json, len);
    }

    return err;
}

const char *autopid_config_path(void)
{
    return AP_CONFIG_PATH;
}
