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
 * @file data_logger.c
 * @brief Lifecycle, the param registry, the PSRAM record rings, and
 *        the writer task (see include/data_logger.h for the model).
 *        Settings live in data_logger_settings.c (standard §4.1). The
 *        writer is the ONLY code that opens files — it follows
 *        external_storage_is_mounted(), so a yanked card just parks
 *        the writer while the rings keep absorbing (drop-oldest).
 *
 * Two streams since 2026-07-09 (TASK addendum): params
 * (dl_<epoch>.<ext>, `format` setting) and CAN frames
 * (can_<epoch>.<ext>, `can_format`/`can_log`), each with its own
 * engine instance, rotation and retention. Separate rings so a CAN
 * flood can never evict param records.
 */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "external_storage.h"
#include "log_manager.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

/* param registry (RAM; registration is allowed before start) */
static dl_param_entry_t s_params[DL_MAX_PARAMS] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object */

/* record rings (PSRAM), producers push under s_lock, writer pops.
 * Params and frames are SEPARATE rings so a busy CAN bus can never
 * evict param records; the frame ring's live length comes from the
 * ring_len setting (static at the cap — caps not rationing). */
typedef struct
{
    dl_record_t *buf;
    uint32_t     cap;
    uint32_t     head;
    uint32_t     tail;
    uint32_t     fill;
    uint32_t     dropped;
} dl_ring_t;

static dl_record_t s_param_slots[DL_RING_LEN] EXT_RAM_BSS_ATTR;
static dl_record_t s_frame_slots[DL_CAN_RING_MAX] EXT_RAM_BSS_ATTR;
static dl_ring_t s_param_ring = { s_param_slots, DL_RING_LEN };
static dl_ring_t s_frame_ring = { s_frame_slots, 2048 };

/* writer task. PSRAM stack per the standard: it only touches the SD
 * card via SDMMC (never internal flash), so the §2 corollary doesn't
 * apply — and 10 KB of internal RAM is exactly what broke the build
 * that kept it internal (bench 2026-07-07: ~2.5 KB internal free =
 * wifi auth AND sdmmc DMA allocations failing). */
static TaskHandle_t s_task;
static StaticTask_t s_tcb;             /* internal: FreeRTOS object */
static StackType_t s_stack[10240] EXT_RAM_BSS_ATTR;

/* ---- streams ---------------------------------------------------------------- */

typedef struct
{
    const dl_engine_t *eng;
    void              *ctx;
    const char        *prefix;    /* DL_PREFIX_PARAM / DL_PREFIX_CAN  */
    bool               frames;    /* record kind this stream carries  */
    bool               active;    /* logging this stream this boot    */
    uint32_t           max_mb;
    uint32_t           max_files;
    dl_ring_t         *ring;
    char               cur_path[80];
    /* per-stream stats */
    char               file[48];
    uint32_t           file_rows;
    uint32_t           files;
    uint32_t           rotations;
    uint32_t           written;
} dl_stream_t;

enum { ST_PARAM = 0, ST_CAN = 1, ST_COUNT = 2 };

/* engine contexts: one per (engine kind × stream) so any format combo
 * can have two files open at once */
static dl_append_ctx_t s_ap_ctx[ST_COUNT];
static dl_sq_ctx_t s_sq_ctx[ST_COUNT];

static dl_stream_t s_stream[ST_COUNT] =
{
    { .prefix = DL_PREFIX_PARAM, .frames = false,
      .ring = &s_param_ring },
    { .prefix = DL_PREFIX_CAN, .frames = true,
      .ring = &s_frame_ring },
};

static data_logger_stats_t s_stats;
static volatile bool s_run;
static volatile bool s_gate = true;  /* logger.enable/disable rules */

/* ---- engine binding (applied from data_logger_settings.c) ------------------ */

static dl_mf4_ctx_t s_mf4_ctx;   /* CAN stream only (schema)          */
static dl_blf_ctx_t s_blf_ctx;

static void bind_engine(int idx, const char *fmt)
{
    dl_stream_t *st = &s_stream[idx];

    if (strcmp(fmt, "csv") == 0)
    {
        st->eng = &dl_engine_csv;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "binary") == 0)
    {
        st->eng = &dl_engine_binary;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "candump") == 0)
    {
        st->eng = &dl_engine_candump;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "asc") == 0)
    {
        st->eng = &dl_engine_asc;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "jsonl") == 0)
    {
        st->eng = &dl_engine_jsonl;
        st->ctx = &s_ap_ctx[idx];
    }
    else if (strcmp(fmt, "mf4") == 0)
    {
        st->eng = &dl_engine_mf4;
        st->ctx = &s_mf4_ctx;
    }
    else if (strcmp(fmt, "blf") == 0)
    {
        st->eng = &dl_engine_blf;
        st->ctx = &s_blf_ctx;
    }
    else
    {
        st->eng = &dl_engine_sqlite;
        st->ctx = &s_sq_ctx[idx];
    }
}

/* on_apply's landing point for the writer-owned knobs: engine binding,
 * rotation/retention, the frame ring's live length, and which streams
 * are active this boot */
void dl_core_apply(const dl_cfg_t *cfg)
{
    bind_engine(ST_PARAM, cfg->format);
    bind_engine(ST_CAN, cfg->can_format);

    s_stream[ST_PARAM].max_mb = cfg->max_file_mb;
    s_stream[ST_PARAM].max_files = cfg->max_files;
    s_stream[ST_CAN].max_mb = cfg->can_max_file_mb;
    s_stream[ST_CAN].max_files = cfg->can_max_files;
    s_frame_ring.cap = cfg->ring_len;

    if (s_frame_ring.cap > DL_CAN_RING_MAX)
    {
        s_frame_ring.cap = DL_CAN_RING_MAX;
    }

    s_stream[ST_PARAM].active = cfg->enabled;
    s_stream[ST_CAN].active = cfg->enabled && cfg->can.log;
}

/* ---- rings ------------------------------------------------------------------ */

static esp_err_t ring_push(dl_ring_t *r, const dl_record_t *rec)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (r->fill == r->cap)
    {
        r->tail = (r->tail + 1) % r->cap; /* drop-oldest */
        r->fill--;
        r->dropped++;

        if ((r->dropped % 256) == 1)
        {
            ESP_LOGW(TAG, "%s ring full (%lu drops)",
                     (r == &s_frame_ring) ? "frame" : "param",
                     (unsigned long)r->dropped);
        }
    }

    r->buf[r->head] = *rec;
    r->head = (r->head + 1) % r->cap;
    r->fill++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static bool ring_pop(dl_ring_t *r, dl_record_t *rec)
{
    bool got = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (r->fill > 0)
    {
        *rec = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
        r->fill--;
        got = true;
    }

    xSemaphoreGive(s_lock);
    return got;
}

esp_err_t dl_frame_push(const dl_record_t *rec)
{
    if (!dl_settings_config()->enabled || !s_stream[ST_CAN].active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return ring_push(&s_frame_ring, rec);
}

/* ---- writer ---------------------------------------------------------------- */

static void params_forget_db_ids(void)
{
    for (int i = 0; i < DL_MAX_PARAMS; i++)
    {
        s_params[i].db_id = 0;
    }
}

static void scan_dir(dl_scan_t *scan, const char *prefix)
{
    DIR *dir = opendir(DL_DIR);

    dl_scan_init(scan, prefix);

    if (dir == NULL)
    {
        return;
    }

    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL)
    {
        dl_scan_add(scan, ent->d_name);
    }

    closedir(dir);
}

static void enforce_retention(dl_stream_t *st)
{
    dl_scan_t scan;

    scan_dir(&scan, st->prefix);
    st->files = (uint32_t)scan.count;

    while (scan.count > (int)st->max_files)
    {
        char path[80];

        snprintf(path, sizeof(path), DL_DIR "/%s", scan.oldest);

        if (unlink(path) != 0)
        {
            ESP_LOGW(TAG, "retention unlink %s failed", path);
            break;
        }

        ESP_LOGI(TAG, "retention: deleted %s", scan.oldest);
        scan_dir(&scan, st->prefix);
        st->files = (uint32_t)scan.count;
    }
}

/* the newest file is resumable only if the current engine wrote it */
static bool ext_matches(const char *fname, const char *ext)
{
    size_t fl = strlen(fname);
    size_t el = strlen(ext);

    return fl > el && strcmp(fname + fl - el, ext) == 0;
}

/* open the newest existing file, or a fresh one when none/rotating */
static esp_err_t open_current(dl_stream_t *st, bool force_new)
{
    dl_scan_t scan;
    char fname[48];

    if (st->eng->single_use)
    {
        force_new = true; /* relative-time / patched-header formats */
    }

    mkdir(DL_DIR, 0775);
    scan_dir(&scan, st->prefix);

    if (!force_new && scan.count > 0 &&
        ext_matches(scan.newest, st->eng->ext))
    {
        strcpy(fname, scan.newest);
    }
    else
    {
        int64_t epoch = (int64_t)time(NULL);
        int64_t newest_epoch = 0;

        /* clock rolled back / rotated within 1 s: keep names monotonic */
        if (scan.count > 0 && dl_files_parse(scan.newest, &newest_epoch) &&
            epoch <= newest_epoch)
        {
            epoch = newest_epoch + 1;
        }

        dl_files_make(fname, sizeof(fname), st->prefix, epoch,
                      st->eng->ext);
    }

    snprintf(st->cur_path, sizeof(st->cur_path), DL_DIR "/%s", fname);

    esp_err_t err = st->eng->open(st->ctx, st->cur_path, st->frames);

    if (err != ESP_OK)
    {
        st->cur_path[0] = '\0';
        return err;
    }

    if (!st->frames)
    {
        params_forget_db_ids();
    }

    snprintf(st->file, sizeof(st->file), "%s", fname);
    st->file_rows = 0;
    enforce_retention(st);
    ESP_LOGI(TAG, "logging to %s (rotate at %lu MB)", fname,
             (unsigned long)st->max_mb);
    return ESP_OK;
}

static void close_current(dl_stream_t *st)
{
    st->eng->close(st->ctx);
    st->cur_path[0] = '\0';
    st->file[0] = '\0';
}

static void close_all(void)
{
    for (int i = 0; i < ST_COUNT; i++)
    {
        if (s_stream[i].eng->is_open(s_stream[i].ctx))
        {
            close_current(&s_stream[i]);
        }
    }
}

static void rotate(dl_stream_t *st)
{
    char rotated[48];

    snprintf(rotated, sizeof(rotated), "%s", st->file);
    close_current(st);
    st->rotations++;

    if (open_current(st, true) == ESP_OK)
    {
        dl_events_rotated(rotated);
    }
}

/* drain up to batch_rows records inside one transaction/flush */
static void write_batch(dl_stream_t *st)
{
    dl_record_t rec;
    uint32_t n = 0;
    bool failed = false;
    uint32_t batch_rows = dl_settings_config()->batch_rows;

    if (st->eng->begin(st->ctx) != ESP_OK)
    {
        failed = true;
    }

    while (!failed && n < batch_rows && ring_pop(st->ring, &rec))
    {
        dl_param_entry_t *p =
            (rec.kind == DL_REC_PARAM) ? &s_params[rec.u.p.param] : NULL;

        if (st->eng->write(st->ctx, &rec, p) != ESP_OK)
        {
            failed = true;
            break;
        }

        n++;
    }

    if (!failed && st->eng->commit(st->ctx) != ESP_OK)
    {
        failed = true;
    }

    if (failed)
    {
        /* card yanked mid-write, disk full, corruption — drop the file
         * handle and let the mount/open path recover next lap */
        s_stats.errors++;
        dl_events_error("write", (int)s_stats.errors);
        close_current(st);
        return;
    }

    st->written += n;
    st->file_rows += n;

    if (st->eng->bytes(st->ctx) >=
        (uint64_t)st->max_mb * 1024u * 1024u)
    {
        rotate(st);
    }
}

static void writer_task(void *arg)
{
    const dl_cfg_t *cfg = dl_settings_config();

    (void)arg;

    while (true)
    {
        if (!s_run || !s_gate)
        {
            /* stopped, or paused by a logger.disable rule — the rings
             * keep absorbing (drop-oldest), so re-enabling flushes
             * the newest records from before the trigger */
            close_all();
            s_stats.storage_ok = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)); /* resume promptly too */
            continue;
        }

        if (!external_storage_is_mounted())
        {
            if (s_stream[ST_PARAM].eng->is_open(s_stream[ST_PARAM].ctx) ||
                s_stream[ST_CAN].eng->is_open(s_stream[ST_CAN].ctx))
            {
                ESP_LOGW(TAG, "card gone; parking");
                close_all();
            }

            s_stats.storage_ok = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool all_open = true;

        for (int i = 0; i < ST_COUNT; i++)
        {
            dl_stream_t *st = &s_stream[i];

            if (st->active && !st->eng->is_open(st->ctx) &&
                open_current(st, false) != ESP_OK)
            {
                s_stats.errors++;
                all_open = false;
            }
        }

        if (!all_open)
        {
            s_stats.storage_ok = false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        s_stats.storage_ok = true;

        if (s_param_ring.fill >= cfg->batch_rows ||
            s_frame_ring.fill >= cfg->batch_rows)
        {
            /* keep draining hot rings without the nap */
            if (s_param_ring.fill >= cfg->batch_rows)
            {
                write_batch(&s_stream[ST_PARAM]);
            }

            if (s_frame_ring.fill >= cfg->batch_rows &&
                s_stream[ST_CAN].active)
            {
                write_batch(&s_stream[ST_CAN]);
            }

            continue;
        }

        /* the nap ends early when the gate flips (dl_runtime_gate notifies):
           a pause — a rule, /api/logger/gate or the export reading the
           active file — closes the files right away instead of up to
           flush_ms later (2026-09-06; the export used to find the newest
           file still locked and return nothing) */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cfg->flush_ms));

        if (!s_run || !s_gate)
        {
            continue; /* gate flipped during the nap — park FIRST, a
                         "paused" logger must not leak a batch */
        }

        if (s_param_ring.fill > 0 && s_stream[ST_PARAM].active)
        {
            write_batch(&s_stream[ST_PARAM]);
        }

        if (s_frame_ring.fill > 0 && s_stream[ST_CAN].active)
        {
            write_batch(&s_stream[ST_CAN]);
        }
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t data_logger_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "data_logger", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    dl_events_register();
    return dl_settings_register();
}

esp_err_t data_logger_start(void)
{
    const dl_cfg_t *cfg = dl_settings_config();

    if (!dl_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    s_stats.enabled = cfg->enabled;
    s_stats.can_enabled = s_stream[ST_CAN].active;

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    s_run = true;

    if (s_task == NULL)
    {
        s_task = xTaskCreateStatic(writer_task, "dl_writer",
                                   sizeof(s_stack) / sizeof(s_stack[0]),
                                   NULL, 3, s_stack, &s_tcb);

        if (s_task == NULL)
        {
            return ESP_FAIL;
        }
    }

    if (s_stream[ST_CAN].active && dl_can_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "CAN drain task failed; can stream off");
        s_stream[ST_CAN].active = false;
        s_stats.can_enabled = false;
    }

    s_stats.running = true;
    ESP_LOGI(TAG, "started (params=%s%s, can=%s%s, batch %lu, "
             "flush %lu ms)", s_stream[ST_PARAM].eng->ext + 1,
             cfg->autopid_log == 2 ? "+autopid.all"
             : cfg->autopid_log == 1 ? "+autopid.changed" : "",
             s_stream[ST_CAN].active ? s_stream[ST_CAN].eng->ext + 1
                                     : "off",
             (s_stream[ST_CAN].active && cfg->can.monitor_all)
                 ? "/all-ids" : "",
             (unsigned long)cfg->batch_rows,
             (unsigned long)cfg->flush_ms);
    return ESP_OK;
}

esp_err_t data_logger_stop(void)
{
    s_run = false; /* writer closes the files on its next lap */
    s_stats.running = false;
    return ESP_OK;
}

/* ---- public API --------------------------------------------------------------- */

esp_err_t data_logger_register_param(const char *source, const char *name,
                                     dl_param_t *out)
{
    if (source == NULL || name == NULL || out == NULL ||
        strlen(source) >= DL_SOURCE_MAX || strlen(name) >= DL_NAME_MAX ||
        source[0] == '\0' || name[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < DL_MAX_PARAMS; i++)
    {
        if (s_params[i].used)
        {
            if (strcmp(s_params[i].source, source) == 0 &&
                strcmp(s_params[i].name, name) == 0)
            {
                *out = (dl_param_t)i;
                err = ESP_OK;
                break;
            }

            continue;
        }

        strcpy(s_params[i].source, source);
        strcpy(s_params[i].name, name);
        s_params[i].db_id = 0;
        s_params[i].used = true;
        *out = (dl_param_t)i;
        err = ESP_OK;
        break;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t data_logger_write_at(dl_param_t param, int64_t epoch_ms,
                               double value)
{
    if (param < 0 || param >= DL_MAX_PARAMS || !s_params[param].used)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!dl_settings_config()->enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    dl_record_t rec =
    {
        .ts_ms = epoch_ms,
        .u.p = { .value = value, .param = param },
        .kind = DL_REC_PARAM,
    };

    return ring_push(&s_param_ring, &rec);
}

esp_err_t data_logger_write(dl_param_t param, double value)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return data_logger_write_at(param,
                                (int64_t)tv.tv_sec * 1000 +
                                    tv.tv_usec / 1000,
                                value);
}

/* autopid value sink — main wires it to autopid_set_value_sink().
 * Poller/filter task context: registry lookup + ring push, no IO. */
void data_logger_autopid_sink(const char *name, const char *unit,
                              double value, bool changed)
{
    const dl_cfg_t *cfg = dl_settings_config();

    (void)unit;

    if (cfg->autopid_log == 0 || (cfg->autopid_log == 1 && !changed) ||
        !cfg->enabled)
    {
        return;
    }

    dl_param_t param;

    if (data_logger_register_param("autopid", name, &param) != ESP_OK)
    {
        return; /* registry full — counted nowhere, params are bounded */
    }

    (void)data_logger_write(param, value);
}

esp_err_t data_logger_stats(data_logger_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_stats;

    /* param stream */
    snprintf(out->file, sizeof(out->file), "%s", s_stream[ST_PARAM].file);
    out->file_rows = s_stream[ST_PARAM].file_rows;
    out->files = s_stream[ST_PARAM].files;
    out->rotations = s_stream[ST_PARAM].rotations;
    out->written = s_stream[ST_PARAM].written;
    out->queued = s_param_ring.fill;
    out->dropped = s_param_ring.dropped;

    /* CAN stream */
    snprintf(out->can_file, sizeof(out->can_file), "%s",
             s_stream[ST_CAN].file);
    out->can_file_rows = s_stream[ST_CAN].file_rows;
    out->can_files = s_stream[ST_CAN].files;
    out->can_rotations = s_stream[ST_CAN].rotations;
    out->frames_written = s_stream[ST_CAN].written;
    out->can_queued = s_frame_ring.fill + dl_can_queued();
    out->frames_dropped = s_frame_ring.dropped;

    out->paused = !s_gate;
    return ESP_OK;
}

void dl_runtime_gate(bool on)
{
    if (s_gate != on)
    {
        ESP_LOGI(TAG, "%s by rule", on ? "enabled" : "paused");
    }

    s_gate = on;

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);   /* wake the writer out of its nap */
    }
}
