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
 * @file autopid_dbc.c
 * @brief DBC file store + PSRAM cache + the add-to-filters merge
 *        (TASK_dbc.md §4-5). Raw .dbc files live at
 *        /data/autopid/dbc/<name>.dbc; each is parsed into PSRAM
 *        message/signal tables at boot + upload. "Add" compiles the
 *        selected signals to expressions, merges them into
 *        /data/autopid/config.json as filter parameters, DRY-RUNS the
 *        result through ap_config_parse, then atomically saves and
 *        live-reloads — nothing is stored on a validation failure.
 *
 * File I/O only from internal-stack contexts (boot = main task,
 * store/delete/add = httpd) — the §2-corollary rule.
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

#define DBC_DIR "/data/autopid/dbc"

typedef struct
{
    bool          used;
    char          name[AP_DTC_DB_NAME_LEN];  /* same name rules        */
    size_t        bytes;
    ap_dbc_msg_t *msgs;
    int           n_msgs;
    ap_dbc_sig_t *sigs;
    int           n_sigs;
} dbc_file_t;

static dbc_file_t s_dbc[AP_DBC_MAX];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

void ap_dbc_init(void)
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
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' &&
            name[i] != '-')
        {
            return false;
        }
    }

    return true;
}

static void slot_free(dbc_file_t *f)
{
    heap_caps_free(f->msgs);
    heap_caps_free(f->sigs);
    memset(f, 0, sizeof(*f));
}

static dbc_file_t *slot_find(const char *name)
{
    for (int i = 0; i < AP_DBC_MAX; i++)
    {
        if (s_dbc[i].used && strcmp(s_dbc[i].name, name) == 0)
        {
            return &s_dbc[i];
        }
    }

    return NULL;
}

/** Parse @p text and install the tables into the named slot. */
static esp_err_t slot_install(const char *name, const char *text,
                              size_t len, size_t file_bytes, char *err,
                              size_t err_len)
{
    ap_dbc_msg_t *msgs = heap_caps_malloc(
        sizeof(ap_dbc_msg_t) * AP_DBC_MSGS_MAX, MALLOC_CAP_SPIRAM);
    ap_dbc_sig_t *sigs = heap_caps_malloc(
        sizeof(ap_dbc_sig_t) * AP_DBC_SIGS_MAX, MALLOC_CAP_SPIRAM);
    int nm = 0;

    if (msgs == NULL || sigs == NULL)
    {
        heap_caps_free(msgs);
        heap_caps_free(sigs);
        snprintf(err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    int ns = ap_dbc_parse(text, len, msgs, AP_DBC_MSGS_MAX, &nm, sigs,
                          AP_DBC_SIGS_MAX, err, err_len);

    if (ns < 0)
    {
        heap_caps_free(msgs);
        heap_caps_free(sigs);
        return ESP_ERR_INVALID_ARG;
    }

    /* shrink to actual */
    ap_dbc_msg_t *tm = heap_caps_malloc(sizeof(ap_dbc_msg_t) * (size_t)nm,
                                        MALLOC_CAP_SPIRAM);
    ap_dbc_sig_t *ts = heap_caps_malloc(sizeof(ap_dbc_sig_t) * (size_t)ns,
                                        MALLOC_CAP_SPIRAM);

    if (tm != NULL)
    {
        memcpy(tm, msgs, sizeof(ap_dbc_msg_t) * (size_t)nm);
        heap_caps_free(msgs);
        msgs = tm;
    }

    if (ts != NULL)
    {
        memcpy(ts, sigs, sizeof(ap_dbc_sig_t) * (size_t)ns);
        heap_caps_free(sigs);
        sigs = ts;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dbc_file_t *f = slot_find(name);

    if (f == NULL)
    {
        for (int i = 0; i < AP_DBC_MAX; i++)
        {
            if (!s_dbc[i].used)
            {
                f = &s_dbc[i];
                break;
            }
        }
    }

    if (f == NULL)
    {
        xSemaphoreGive(s_lock);
        heap_caps_free(msgs);
        heap_caps_free(sigs);
        snprintf(err, err_len, "all %d DBC slots in use", AP_DBC_MAX);
        return ESP_ERR_NO_MEM;
    }

    if (f->used)
    {
        slot_free(f);
    }

    f->used = true;
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->bytes = file_bytes;
    f->msgs = msgs;
    f->n_msgs = nm;
    f->sigs = sigs;
    f->n_sigs = ns;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "dbc '%s': %d messages / %d signals cached", name, nm,
             ns);
    return ESP_OK;
}

static esp_err_t dbc_list_cb(const char *fname, bool is_dir, size_t size,
                             void *arg)
{
    (void)arg;

    size_t n = strlen(fname);

    if (is_dir || n <= 4 || strcmp(fname + n - 4, ".dbc") != 0 ||
        n - 4 >= AP_DTC_DB_NAME_LEN)
    {
        return ESP_OK;
    }

    char base[AP_DTC_DB_NAME_LEN];
    char path[64];

    memcpy(base, fname, n - 4);
    base[n - 4] = '\0';

    if (!name_ok(base) || size == 0 || size > AP_DBC_FILE_MAX)
    {
        return ESP_OK;
    }

    snprintf(path, sizeof(path), DBC_DIR "/%s", fname);

    char *buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    size_t got = 0;

    if (buf != NULL &&
        filesystem_read(path, buf, size, &got) == ESP_OK && got == size)
    {
        char err[96];

        buf[size] = '\0';
        (void)slot_install(base, buf, size, size, err, sizeof(err));
    }

    heap_caps_free(buf);
    return ESP_OK;
}

void ap_dbc_load_all(void)
{
    ap_dbc_init();
    (void)filesystem_list(DBC_DIR, dbc_list_cb, NULL);
}

/* ---- store / delete -------------------------------------------------------------- */

esp_err_t ap_dbc_store(const char *name, const char *raw, size_t raw_len,
                       int *msgs_out, int *sigs_out, char *err,
                       size_t err_len)
{
    if (!name_ok(name))
    {
        snprintf(err, err_len, "bad name (A-Za-z0-9_-, max %d)",
                 AP_DTC_DB_NAME_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (raw_len == 0 || raw_len > AP_DBC_FILE_MAX)
    {
        snprintf(err, err_len, "size 1..%d bytes", AP_DBC_FILE_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t res = slot_install(name, raw, raw_len, raw_len, err,
                                 err_len);

    if (res != ESP_OK)
    {
        return res;
    }

    char path[64];

    snprintf(path, sizeof(path), DBC_DIR "/%s.dbc", name);
    (void)filesystem_mkdirs(DBC_DIR);

    if (filesystem_write(path, raw, raw_len) != ESP_OK)
    {
        snprintf(err, err_len, "store failed");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dbc_file_t *f = slot_find(name);

    if (f != NULL)
    {
        if (msgs_out != NULL)
        {
            *msgs_out = f->n_msgs;
        }

        if (sigs_out != NULL)
        {
            *sigs_out = f->n_sigs;
        }
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ap_dbc_delete(const char *name)
{
    char path[64];

    if (!name_ok(name))
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dbc_file_t *f = slot_find(name);

    if (f != NULL)
    {
        slot_free(f);
    }

    xSemaphoreGive(s_lock);
    snprintf(path, sizeof(path), DBC_DIR "/%s.dbc", name);
    return (filesystem_delete(path) == ESP_OK || f != NULL)
               ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---- browse ------------------------------------------------------------------------ */

static dbc_file_t *next_by_name(const char *after)
{
    dbc_file_t *best = NULL;

    for (int i = 0; i < AP_DBC_MAX; i++)
    {
        if (!s_dbc[i].used)
        {
            continue;
        }

        if (after != NULL && strcmp(s_dbc[i].name, after) <= 0)
        {
            continue;
        }

        if (best == NULL || strcmp(s_dbc[i].name, best->name) < 0)
        {
            best = &s_dbc[i];
        }
    }

    return best;
}

cJSON *ap_dbc_list_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "dbcs");

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (dbc_file_t *f = next_by_name(NULL); f != NULL && arr != NULL;
         f = next_by_name(f->name))
    {
        cJSON *e = cJSON_CreateObject();

        cJSON_AddStringToObject(e, "name", f->name);
        cJSON_AddNumberToObject(e, "messages", f->n_msgs);
        cJSON_AddNumberToObject(e, "signals", f->n_sigs);
        cJSON_AddNumberToObject(e, "bytes", (double)f->bytes);
        cJSON_AddItemToArray(arr, e);
    }

    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(o, "max", AP_DBC_MAX);
    return o;
}

static bool str_imatch(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), hl = strlen(hay);

    if (nl == 0)
    {
        return true;
    }

    if (nl > hl)
    {
        return false;
    }

    for (size_t i = 0; i + nl <= hl; i++)
    {
        size_t j = 0;

        while (j < nl && tolower((unsigned char)hay[i + j]) ==
                             tolower((unsigned char)needle[j]))
        {
            j++;
        }

        if (j == nl)
        {
            return true;
        }
    }

    return false;
}

cJSON *ap_dbc_signals_json(const char *db, const char *q, int offset,
                           int limit)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "items");
    int total = 0, emitted = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (dbc_file_t *f = next_by_name(NULL); f != NULL && arr != NULL;
         f = next_by_name(f->name))
    {
        if (db != NULL && db[0] != '\0' && strcmp(f->name, db) != 0)
        {
            continue;
        }

        for (int i = 0; i < f->n_sigs; i++)
        {
            const ap_dbc_sig_t *s = &f->sigs[i];
            const ap_dbc_msg_t *m = &f->msgs[s->msg];

            if (q != NULL && q[0] != '\0' && !str_imatch(s->name, q) &&
                !str_imatch(m->name, q))
            {
                continue;
            }

            if (total >= offset && emitted < limit)
            {
                cJSON *e = cJSON_CreateObject();
                char expr[AP_EXPR_LEN];
                char mexpr[AP_MUX_EXPR_LEN];
                float mval = 0;
                const char *reason = NULL;

                cJSON_AddStringToObject(e, "db", f->name);
                cJSON_AddStringToObject(e, "msg", m->name);
                cJSON_AddNumberToObject(e, "id", m->id);
                cJSON_AddStringToObject(e, "name", s->name);
                cJSON_AddStringToObject(e, "unit", s->unit);
                cJSON_AddNumberToObject(e, "start", s->start);
                cJSON_AddNumberToObject(e, "len", s->len);
                cJSON_AddStringToObject(e, "order",
                                        s->intel ? "intel" : "motorola");
                cJSON_AddBoolToObject(e, "signed", s->is_signed);
                cJSON_AddNumberToObject(e, "factor", s->factor);
                cJSON_AddNumberToObject(e, "offset", s->offset);

                if (s->max > s->min)
                {
                    cJSON_AddNumberToObject(e, "min", s->min);
                    cJSON_AddNumberToObject(e, "max", s->max);
                }

                if (ap_dbc_expr(s, expr, sizeof(expr), &reason) ==
                        ESP_OK &&
                    ap_dbc_mux_cond(s, f->msgs, f->sigs, f->n_sigs,
                                    mexpr, sizeof(mexpr), &mval,
                                    &reason) == ESP_OK)
                {
                    cJSON_AddBoolToObject(e, "supported", true);
                    cJSON_AddStringToObject(e, "expression", expr);

                    if (mexpr[0] != '\0')
                    {
                        cJSON_AddStringToObject(e, "mux_expr", mexpr);
                        cJSON_AddNumberToObject(e, "mux_val", mval);
                    }
                }
                else
                {
                    cJSON_AddBoolToObject(e, "supported", false);
                    cJSON_AddStringToObject(e, "reason", reason);
                }

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

/* ---- add to filters ---------------------------------------------------------------- */

static bool param_name_taken(const cJSON *root, const char *name)
{
    static const char *const LISTS[2] = { "pids", "filters" };

    for (int l = 0; l < 2; l++)
    {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root,
                                                            LISTS[l]);
        const cJSON *entry;

        cJSON_ArrayForEach(entry, arr)
        {
            const cJSON *params = cJSON_GetObjectItemCaseSensitive(
                entry, "parameters");
            const cJSON *prm;

            cJSON_ArrayForEach(prm, params)
            {
                const cJSON *pn = cJSON_GetObjectItemCaseSensitive(
                    prm, "name");

                if (cJSON_IsString(pn) &&
                    strcmp(pn->valuestring, name) == 0)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

static cJSON *find_filter(cJSON *root, uint32_t frame_id)
{
    cJSON *filters = cJSON_GetObjectItemCaseSensitive(root, "filters");
    cJSON *f;

    cJSON_ArrayForEach(f, filters)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(f, "frame_id");

        if (cJSON_IsNumber(id) && (uint32_t)id->valuedouble == frame_id)
        {
            return f;
        }
    }

    return NULL;
}

static void ensure_group(cJSON *root, const char *group)
{
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(root, "groups");
    cJSON *g;

    if (groups == NULL)
    {
        groups = cJSON_AddArrayToObject(root, "groups");
    }

    cJSON_ArrayForEach(g, groups)
    {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(g, "name");

        if (cJSON_IsString(n) && strcmp(n->valuestring, group) == 0)
        {
            return;
        }
    }

    cJSON *ng = cJSON_CreateObject();

    cJSON_AddStringToObject(ng, "name", group);
    cJSON_AddBoolToObject(ng, "enabled", true);
    cJSON_AddNumberToObject(ng, "period_ms", 1000);
    cJSON_AddItemToArray(groups, ng);
}

esp_err_t ap_dbc_add(const char *db, const cJSON *signals,
                     const char *group, int monitor_ms, int period_ms,
                     cJSON **result, char *err, size_t err_len)
{
    if (group == NULL || group[0] == '\0')
    {
        group = "default";
    }

    if (monitor_ms <= 0)
    {
        monitor_ms = 1000;
    }

    /* current config text (missing file = empty default) */
    size_t csize = 0;
    char *ctext = NULL;

    if (filesystem_size(autopid_config_path(), &csize) == ESP_OK &&
        csize > 0)
    {
        ctext = heap_caps_malloc(csize + 1, MALLOC_CAP_SPIRAM);

        size_t got = 0;

        if (ctext == NULL ||
            filesystem_read(autopid_config_path(), ctext, csize, &got)
                    != ESP_OK ||
            got != csize)
        {
            heap_caps_free(ctext);
            snprintf(err, err_len, "config read failed");
            return ESP_FAIL;
        }

        ctext[csize] = '\0';
    }

    cJSON *root = (ctext != NULL)
                      ? cJSON_Parse(ctext)
                      : cJSON_Parse("{\"groups\":[],\"pids\":[],"
                                    "\"filters\":[]}");

    heap_caps_free(ctext);

    if (root == NULL)
    {
        snprintf(err, err_len, "config parse failed");
        return ESP_FAIL;
    }

    if (cJSON_GetObjectItemCaseSensitive(root, "filters") == NULL)
    {
        cJSON_AddArrayToObject(root, "filters");
    }

    ensure_group(root, group);

    cJSON *out = cJSON_CreateObject();
    cJSON *skipped = cJSON_AddArrayToObject(out, "skipped");
    int added = 0;
    const cJSON *req;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dbc_file_t *f = slot_find(db != NULL ? db : "");

    if (f == NULL)
    {
        xSemaphoreGive(s_lock);
        cJSON_Delete(root);
        cJSON_Delete(out);
        snprintf(err, err_len, "no such DBC");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON_ArrayForEach(req, signals)
    {
        const char *want = NULL;
        double want_id = -1;

        if (cJSON_IsString(req))
        {
            want = req->valuestring;
        }
        else
        {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(req,
                                                              "name");
            const cJSON *i = cJSON_GetObjectItemCaseSensitive(req, "id");

            if (cJSON_IsString(n))
            {
                want = n->valuestring;
            }

            if (cJSON_IsNumber(i))
            {
                want_id = i->valuedouble;
            }
        }

        const char *skip_reason = NULL;
        const ap_dbc_sig_t *sig = NULL;

        for (int i = 0; want != NULL && i < f->n_sigs; i++)
        {
            if (strcmp(f->sigs[i].name, want) != 0)
            {
                continue;
            }

            if (want_id >= 0 &&
                f->msgs[f->sigs[i].msg].id != (uint32_t)want_id)
            {
                continue;
            }

            sig = &f->sigs[i];
            break;
        }

        char expr[AP_EXPR_LEN];
        char mexpr[AP_MUX_EXPR_LEN] = "";
        float mval = 0;

        if (want == NULL)
        {
            want = "?";
            skip_reason = "missing name";
        }
        else if (sig == NULL)
        {
            skip_reason = "not in this DBC";
        }
        else if (ap_dbc_expr(sig, expr, sizeof(expr), &skip_reason)
                     != ESP_OK ||
                 ap_dbc_mux_cond(sig, f->msgs, f->sigs, f->n_sigs,
                                 mexpr, sizeof(mexpr), &mval,
                                 &skip_reason) != ESP_OK)
        {
            /* skip_reason set by the compiler */
        }
        else
        {
            /* global param-name uniqueness (config invariant) */
            char pname[AP_NAME_LEN];

            /* DBC identifiers (<=32 chars) truncate into the config's
             * 31-char parameter names */
            snprintf(pname, sizeof(pname), "%.31s", sig->name);

            if (param_name_taken(root, pname))
            {
                /* 21 name chars + '_' + <=8 hex + NUL = 31 <= cap */
                snprintf(pname, sizeof(pname), "%.21s_%X", sig->name,
                         (unsigned)f->msgs[sig->msg].id);
            }

            if (param_name_taken(root, pname))
            {
                skip_reason = "parameter name already in config";
            }
            else
            {
                uint32_t fid = f->msgs[sig->msg].id;
                cJSON *flt = find_filter(root, fid);

                if (flt == NULL)
                {
                    flt = cJSON_CreateObject();
                    cJSON_AddNumberToObject(flt, "frame_id", fid);
                    cJSON_AddNumberToObject(flt, "monitor_ms",
                                            monitor_ms);
                    cJSON_AddNumberToObject(flt, "period_ms",
                                            period_ms > 0 ? period_ms
                                                          : 0);
                    cJSON_AddStringToObject(flt, "group", group);
                    cJSON_AddArrayToObject(flt, "parameters");
                    cJSON_AddItemToArray(
                        cJSON_GetObjectItemCaseSensitive(root,
                                                         "filters"),
                        flt);
                }

                cJSON *params = cJSON_GetObjectItemCaseSensitive(
                    flt, "parameters");

                if (cJSON_GetArraySize(params) >= AP_PARAMS_PER)
                {
                    skip_reason = "filter parameter slots full";
                }
                else
                {
                    cJSON *prm = cJSON_CreateObject();

                    cJSON_AddStringToObject(prm, "name", pname);
                    cJSON_AddStringToObject(prm, "expression", expr);

                    if (mexpr[0] != '\0')
                    {
                        cJSON_AddStringToObject(prm, "mux_expr", mexpr);
                        cJSON_AddNumberToObject(prm, "mux_val", mval);
                    }

                    if (sig->unit[0] != '\0')
                    {
                        cJSON_AddStringToObject(prm, "unit", sig->unit);
                    }

                    if (sig->max > sig->min)
                    {
                        cJSON_AddNumberToObject(prm, "min", sig->min);
                        cJSON_AddNumberToObject(prm, "max", sig->max);
                    }

                    cJSON_AddItemToArray(params, prm);
                    added++;
                }
            }
        }

        if (skip_reason != NULL)
        {
            cJSON *sk = cJSON_CreateObject();

            cJSON_AddStringToObject(sk, "name", want);
            cJSON_AddStringToObject(sk, "reason", skip_reason);
            cJSON_AddItemToArray(skipped, sk);
        }
    }

    xSemaphoreGive(s_lock);

    if (added == 0)
    {
        cJSON_Delete(root);
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddNumberToObject(out, "added", 0);
        *result = out;
        return ESP_OK;      /* nothing to save; skips explain why       */
    }

    /* dry-run validate, then atomic save + LIVE reload */
    char *body = cJSON_PrintUnformatted(root);
    int n_filters =
        cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,
                                                            "filters"));

    cJSON_Delete(root);

    if (body == NULL)
    {
        cJSON_Delete(out);
        snprintf(err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    static ap_config_t s_dry EXT_RAM_BSS_ATTR;  /* 400+ KB — not stack  */
    char perr[96];

    if (ap_config_parse(body, &s_dry, perr, sizeof(perr)) != ESP_OK)
    {
        cJSON_free(body);
        cJSON_Delete(out);
        snprintf(err, err_len, "merged config invalid: %s", perr);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t werr = autopid_config_save(body, strlen(body));

    cJSON_free(body);

    if (werr != ESP_OK)
    {
        cJSON_Delete(out);
        snprintf(err, err_len, "config save failed");
        return ESP_FAIL;
    }

    (void)autopid_reload_config();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "added", added);
    cJSON_AddNumberToObject(out, "filters", n_filters);
    *result = out;
    ESP_LOGI(TAG, "dbc add: %d parameter(s) from '%s' merged (filters "
                  "now %d)", added, db, n_filters);
    return ESP_OK;
}
