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
 * @file autopid_dtc_db.c
 * @brief DTC-database store + PSRAM cache (TASK_dtc_db.md §3): uploads
 *        are normalized by the pure importer and stored at
 *        /data/autopid/dtc_db/<name>.db; every db is fully cached in
 *        PSRAM with a sorted index, so lookups (report enrichment,
 *        search, Berry) never touch flash.
 *
 * File I/O happens only in internal-stack contexts (autopid_start =
 * main task; store/delete = httpd task) — the §2-corollary rule.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"
#include "filesystem.h"

#include "autopid.h"
#include "autopid_private.h"

static const char *TAG = "autopid";

#define DB_DIR "/data/autopid/dtc_db"

typedef struct
{
    bool     used;
    char     name[AP_DTC_DB_NAME_LEN];
    char    *buf;                   /* canonical content (PSRAM heap)   */
    size_t   len;
    ap_dtc_db_item_t *items;        /* sorted index (PSRAM heap)        */
    int      n;
} dtc_db_t;

static dtc_db_t s_dbs[AP_DTC_DB_MAX];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

void ap_dtc_db_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

static bool name_ok(const char *name)
{
    size_t n = (name != NULL) ? strlen(name) : 0;

    if (n == 0 || n >= AP_DTC_DB_NAME_LEN)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        char c = name[i];

        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
        {
            return false;
        }
    }

    return true;
}

static void slot_free(dtc_db_t *db)
{
    heap_caps_free(db->buf);
    heap_caps_free(db->items);
    memset(db, 0, sizeof(*db));
}

static dtc_db_t *slot_find(const char *name)
{
    for (int i = 0; i < AP_DTC_DB_MAX; i++)
    {
        if (s_dbs[i].used && strcmp(s_dbs[i].name, name) == 0)
        {
            return &s_dbs[i];
        }
    }

    return NULL;
}

/** Load one canonical .db file into a slot (replacing any same-name
 *  cache). Caller context must allow flash reads (internal stack). */
static esp_err_t slot_load(const char *name)
{
    char path[64];
    size_t size = 0;

    snprintf(path, sizeof(path), DB_DIR "/%s.db", name);

    if (filesystem_size(path, &size) != ESP_OK || size == 0 ||
        size > AP_DTC_DB_FILE_MAX + (AP_DTC_DB_ENTRIES_MAX * 8))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    ap_dtc_db_item_t *items = heap_caps_malloc(
        sizeof(ap_dtc_db_item_t) * AP_DTC_DB_ENTRIES_MAX,
        MALLOC_CAP_SPIRAM);
    size_t got = 0;
    int n = -1;

    if (buf != NULL && items != NULL &&
        filesystem_read(path, buf, size, &got) == ESP_OK && got == size)
    {
        buf[size] = '\0';
        n = ap_dtc_db_index(buf, size, items, AP_DTC_DB_ENTRIES_MAX);
    }

    if (n < 0)
    {
        heap_caps_free(buf);
        heap_caps_free(items);
        ESP_LOGW(TAG, "dtc db '%s': unreadable/corrupt", name);
        return ESP_FAIL;
    }

    /* shrink the index to what's used */
    ap_dtc_db_item_t *tight = heap_caps_malloc(
        sizeof(ap_dtc_db_item_t) * (size_t)n, MALLOC_CAP_SPIRAM);

    if (tight != NULL)
    {
        memcpy(tight, items, sizeof(ap_dtc_db_item_t) * (size_t)n);
        heap_caps_free(items);
        items = tight;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dtc_db_t *db = slot_find(name);

    if (db == NULL)
    {
        for (int i = 0; i < AP_DTC_DB_MAX; i++)
        {
            if (!s_dbs[i].used)
            {
                db = &s_dbs[i];
                break;
            }
        }
    }

    if (db == NULL)
    {
        xSemaphoreGive(s_lock);
        heap_caps_free(buf);
        heap_caps_free(items);
        return ESP_ERR_NO_MEM;
    }

    if (db->used)
    {
        slot_free(db);
    }

    db->used = true;
    snprintf(db->name, sizeof(db->name), "%s", name);
    db->buf = buf;
    db->len = size;
    db->items = items;
    db->n = n;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "dtc db '%s': %d entries cached", name, n);
    return ESP_OK;
}

static esp_err_t list_cb(const char *name, bool is_dir, size_t size,
                         void *arg)
{
    (void)size;
    (void)arg;

    size_t n = strlen(name);

    if (!is_dir && n > 3 && strcmp(name + n - 3, ".db") == 0 &&
        n - 3 < AP_DTC_DB_NAME_LEN)
    {
        char base[AP_DTC_DB_NAME_LEN];

        memcpy(base, name, n - 3);
        base[n - 3] = '\0';

        if (name_ok(base))
        {
            (void)slot_load(base);
        }
    }

    return ESP_OK;
}

void ap_dtc_db_load_all(void)
{
    ap_dtc_db_init();
    (void)filesystem_list(DB_DIR, list_cb, NULL);
}

/* ---- store / delete (httpd context — internal stack) ---------------------------- */

esp_err_t ap_dtc_db_store(const char *name, const char *raw,
                          size_t raw_len, int *entries_out,
                          char fmt_out[12], char *err, size_t err_len)
{
    if (!name_ok(name))
    {
        snprintf(err, err_len, "bad name (A-Za-z0-9_-, max %d)",
                 AP_DTC_DB_NAME_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (raw_len == 0 || raw_len > AP_DTC_DB_FILE_MAX)
    {
        snprintf(err, err_len, "size 1..%d bytes", AP_DTC_DB_FILE_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    /* slot availability (replace keeps its slot) */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool replacing = (slot_find(name) != NULL);
    int used = 0;

    for (int i = 0; i < AP_DTC_DB_MAX; i++)
    {
        used += s_dbs[i].used ? 1 : 0;
    }

    xSemaphoreGive(s_lock);

    if (!replacing && used >= AP_DTC_DB_MAX)
    {
        snprintf(err, err_len, "all %d database slots in use",
                 AP_DTC_DB_MAX);
        return ESP_ERR_NO_MEM;
    }

    size_t out_cap = raw_len + 32 +
                     (size_t)AP_DTC_DB_ENTRIES_MAX * 8;
    /* the emitter checks used + AP_DTC_DESC_MAX headroom BEFORE every
     * entry — cap must cover worst-case content (<= raw_len) PLUS that
     * final-entry headroom, or small files reject their tail */
    size_t scratch_cap = raw_len + AP_DTC_DESC_MAX + 16;
    char *scratch = heap_caps_malloc(scratch_cap, MALLOC_CAP_SPIRAM);
    char *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
    ap_dtc_db_item_t *items = heap_caps_malloc(
        sizeof(ap_dtc_db_item_t) * AP_DTC_DB_ENTRIES_MAX,
        MALLOC_CAP_SPIRAM);
    esp_err_t result = ESP_FAIL;

    if (scratch == NULL || out == NULL || items == NULL)
    {
        snprintf(err, err_len, "out of memory");
        goto done;
    }

    int n = ap_dtc_db_import(raw, raw_len, scratch, scratch_cap, items,
                             AP_DTC_DB_ENTRIES_MAX, fmt_out, err,
                             err_len);

    if (n < 0)
    {
        result = ESP_ERR_INVALID_ARG;
        goto done;
    }

    size_t w = ap_dtc_db_serialize(scratch, items, n, out, out_cap);

    if (w == 0)
    {
        snprintf(err, err_len, "serialize overflow");
        goto done;
    }

    char path[64];

    snprintf(path, sizeof(path), DB_DIR "/%s.db", name);
    (void)filesystem_mkdirs(DB_DIR);

    if (filesystem_write(path, out, w) != ESP_OK)
    {
        snprintf(err, err_len, "store failed");
        goto done;
    }

    result = slot_load(name);

    if (result != ESP_OK)
    {
        snprintf(err, err_len, "cache reload failed");
    }
    else if (entries_out != NULL)
    {
        *entries_out = n;
    }

done:
    heap_caps_free(scratch);
    heap_caps_free(out);
    heap_caps_free(items);
    return result;
}

esp_err_t ap_dtc_db_delete(const char *name)
{
    char path[64];

    if (!name_ok(name))
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dtc_db_t *db = slot_find(name);

    if (db != NULL)
    {
        slot_free(db);
    }

    xSemaphoreGive(s_lock);
    snprintf(path, sizeof(path), DB_DIR "/%s.db", name);
    return (filesystem_delete(path) == ESP_OK || db != NULL)
               ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---- lookup / search / list (no flash — PSRAM cache only) ----------------------- */

/** Iterate used slots in NAME order (deterministic priority). */
static dtc_db_t *next_db_by_name(const char *after)
{
    dtc_db_t *best = NULL;

    for (int i = 0; i < AP_DTC_DB_MAX; i++)
    {
        if (!s_dbs[i].used)
        {
            continue;
        }

        if (after != NULL && strcmp(s_dbs[i].name, after) <= 0)
        {
            continue;
        }

        if (best == NULL || strcmp(s_dbs[i].name, best->name) < 0)
        {
            best = &s_dbs[i];
        }
    }

    return best;
}

static const ap_dtc_db_item_t *db_bsearch(const dtc_db_t *db,
                                          const char *code)
{
    int lo = 0, hi = db->n - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        int c = memcmp(code, db->items[mid].code, 5);

        if (c == 0)
        {
            return &db->items[mid];
        }

        if (c < 0)
        {
            hi = mid - 1;
        }
        else
        {
            lo = mid + 1;
        }
    }

    return NULL;
}

bool ap_dtc_db_lookup(const char *code, char *out, size_t out_cap)
{
    bool found = false;

    if (s_lock == NULL || code == NULL || strlen(code) != 5)
    {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (dtc_db_t *db = next_db_by_name(NULL); db != NULL && !found;
         db = next_db_by_name(db->name))
    {
        const ap_dtc_db_item_t *it = db_bsearch(db, code);

        if (it != NULL)
        {
            size_t n = (it->len < out_cap - 1) ? it->len : out_cap - 1;

            memcpy(out, db->buf + it->off, n);
            out[n] = '\0';
            found = true;
        }
    }

    xSemaphoreGive(s_lock);
    return found;
}

cJSON *ap_dtc_db_list_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "dbs");

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (dtc_db_t *db = next_db_by_name(NULL); db != NULL && arr != NULL;
         db = next_db_by_name(db->name))
    {
        cJSON *e = cJSON_CreateObject();

        cJSON_AddStringToObject(e, "name", db->name);
        cJSON_AddNumberToObject(e, "entries", db->n);
        cJSON_AddNumberToObject(e, "bytes", (double)db->len);
        cJSON_AddItemToArray(arr, e);
    }

    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(o, "max", AP_DTC_DB_MAX);
    return o;
}

cJSON *ap_dtc_db_search_json(const char *q, const char *db_name,
                             int offset, int limit)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "items");
    int total = 0, emitted = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (dtc_db_t *db = next_db_by_name(NULL); db != NULL && arr != NULL;
         db = next_db_by_name(db->name))
    {
        if (db_name != NULL && db_name[0] != '\0' &&
            strcmp(db->name, db_name) != 0)
        {
            continue;
        }

        for (int i = 0; i < db->n; i++)
        {
            const ap_dtc_db_item_t *it = &db->items[i];

            if (!ap_dtc_db_match(it->code, db->buf + it->off, it->len,
                                 q))
            {
                continue;
            }

            if (total >= offset && emitted < limit)
            {
                cJSON *e = cJSON_CreateObject();
                char desc[AP_DTC_DESC_MAX + 1];
                size_t n = it->len;

                memcpy(desc, db->buf + it->off, n);
                desc[n] = '\0';
                cJSON_AddStringToObject(e, "code", it->code);
                cJSON_AddStringToObject(e, "desc", desc);
                cJSON_AddStringToObject(e, "db", db->name);
                cJSON_AddItemToArray(arr, e);
                emitted++;
            }

            total++;
        }
    }

    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(o, "total", total);
    return o;
}

/** Add a {"CODE":"desc"} map for every report code with a db hit. */
void ap_dtc_db_desc_map(cJSON *parent, const char *key,
                        const ap_dtc_report_t *r)
{
    cJSON *map = NULL;
    char desc[AP_DTC_DESC_MAX + 1];
    const char (*sets[3])[AP_DTC_CODE_LEN] =
    {
        r->stored, r->pending, r->permanent,
    };
    const uint8_t counts[3] = { r->n_stored, r->n_pending,
                                r->n_permanent };

    for (int s = 0; s < 3; s++)
    {
        for (uint8_t i = 0; i < counts[s]; i++)
        {
            if (!ap_dtc_db_lookup(sets[s][i], desc, sizeof(desc)))
            {
                continue;
            }

            if (map == NULL)
            {
                map = cJSON_AddObjectToObject(parent, key);
            }

            if (map != NULL &&
                cJSON_GetObjectItemCaseSensitive(map, sets[s][i]) == NULL)
            {
                cJSON_AddStringToObject(map, sets[s][i], desc);
            }
        }
    }
}

/* ---- public wrapper (Berry binding surface) -------------------------------------- */

esp_err_t autopid_dtc_desc(const char *code, char *out, size_t out_cap)
{
    if (code == NULL || out == NULL || out_cap == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char norm[AP_DTC_CODE_LEN];
    uint8_t hi, lo;

    if (!ap_dtc_unformat(code, &hi, &lo))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ap_dtc_format(hi, lo, norm);
    return ap_dtc_db_lookup(norm, out, out_cap) ? ESP_OK
                                                : ESP_ERR_NOT_FOUND;
}
