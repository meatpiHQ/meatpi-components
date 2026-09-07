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
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "external_storage.h"
#include "log_manager.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

/* param registry (RAM; registration is allowed before start) */
/* the registry lives in the PSRAM .noinit envelope below (s_params) */
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

/* ---- PSRAM .noinit envelope (2026-09-07, ROBUSTNESS.md case 6) ------------
   The registry and both rings survive warm resets: after a panic, a
   watchdog or a restart that skipped the clean stop, the next boot
   validates this envelope (magic/version/caps, a CRC over the registry
   names, index + per-record sanity — data_logger_recover.c) and writes
   the records that were still queued BEFORE anything new. Power cuts and
   EN-pin resets leave random or bit-rotten PSRAM; the envelope catches
   that and starts clean. Same pattern as restart_tracker / log_manager's
   ring, including the 64-byte MSPI tuning guard that must lead the object. */
#define DL_PS_MAGIC   0x53504C44u /* "DLPS" */
#define DL_PS_VERSION 1u

typedef struct
{
    uint8_t          guard[64];
    uint32_t         magic;
    uint32_t         version;
    uint32_t         param_cap;
    uint32_t         frame_cap;
    uint32_t         reg_crc;          /* used + names of params[]         */
    dl_ring_t        param_ring;
    dl_ring_t        frame_ring;
    dl_param_entry_t params[DL_MAX_PARAMS];
    dl_record_t      param_slots[DL_RING_LEN];
    dl_record_t      frame_slots[DL_CAN_RING_MAX];
} dl_psram_t;

static dl_psram_t s_ps EXT_RAM_NOINIT_ATTR;
#define s_params     (s_ps.params)
#define s_param_ring (s_ps.param_ring)
#define s_frame_ring (s_ps.frame_ring)

static uint32_t reg_crc_calc(void)
{
    uint32_t crc = 0;

    for (int i = 0; i < DL_MAX_PARAMS; i++)
    {
        const dl_param_entry_t *p = &s_ps.params[i];
        uint8_t used = p->used ? 1 : 0;

        crc = dl_recover_crc32_update(crc, &used, 1);
        crc = dl_recover_crc32_update(crc, p->source, sizeof(p->source));
        crc = dl_recover_crc32_update(crc, p->name, sizeof(p->name));
    }

    return crc;
}

static void ps_reset(void)
{
    memset(&s_ps.magic, 0, sizeof(s_ps) - offsetof(dl_psram_t, magic));
    s_ps.magic = DL_PS_MAGIC;
    s_ps.version = DL_PS_VERSION;
    s_ps.param_cap = DL_RING_LEN;
    s_ps.frame_cap = DL_CAN_RING_MAX;
    s_ps.param_ring.buf = s_ps.param_slots;
    s_ps.param_ring.cap = DL_RING_LEN;
    s_ps.frame_ring.buf = s_ps.frame_slots;
    s_ps.frame_ring.cap = 2048; /* live length set by dl_core_apply */
    s_ps.reg_crc = reg_crc_calc();
}

/* drop everything from the first record that does not look like one */
static void ring_trim(dl_ring_t *r)
{
    for (uint32_t i = 0; i < r->fill; i++)
    {
        if (!dl_recover_record_sane(&r->buf[(r->tail + i) % r->cap]))
        {
            r->fill = i;
            r->head = (r->tail + i) % r->cap;
            return;
        }
    }
}

/* returns the number of records carried over from before a warm reset */
static uint32_t ps_adopt(void)
{
    bool ok = s_ps.magic == DL_PS_MAGIC && s_ps.version == DL_PS_VERSION &&
              s_ps.param_cap == DL_RING_LEN &&
              s_ps.frame_cap == DL_CAN_RING_MAX &&
              s_ps.reg_crc == reg_crc_calc() &&
              dl_recover_ring_sane(s_ps.param_ring.cap, DL_RING_LEN,
                                   s_ps.param_ring.head, s_ps.param_ring.tail,
                                   &s_ps.param_ring.fill) &&
              dl_recover_ring_sane(s_ps.frame_ring.cap, DL_CAN_RING_MAX,
                                   s_ps.frame_ring.head, s_ps.frame_ring.tail,
                                   &s_ps.frame_ring.fill);

    for (int i = 0; ok && i < DL_MAX_PARAMS; i++)
    {
        const dl_param_entry_t *p = &s_ps.params[i];

        if (p->used &&
            (strnlen(p->source, DL_SOURCE_MAX) >= DL_SOURCE_MAX ||
             strnlen(p->name, DL_NAME_MAX) >= DL_NAME_MAX ||
             p->source[0] == '\0' || p->name[0] == '\0'))
        {
            ok = false;
        }
    }

    if (!ok)
    {
        ps_reset();
        return 0;
    }

    s_ps.param_ring.buf = s_ps.param_slots;
    s_ps.frame_ring.buf = s_ps.frame_slots;
    ring_trim(&s_ps.param_ring);
    ring_trim(&s_ps.frame_ring);
    s_ps.param_ring.dropped = 0;
    s_ps.frame_ring.dropped = 0;

    for (int i = 0; i < DL_MAX_PARAMS; i++)
    {
        s_ps.params[i].db_id = 0; /* interned per open file */
    }

    return s_ps.param_ring.fill + s_ps.frame_ring.fill;
}

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
    /* ROBUSTNESS.md */
    uint32_t           open_fails;    /* consecutive open failures        */
    uint32_t           corrupt_files; /* *.corrupt set aside for this stream */
    char               last_try[48];  /* file name of the last open attempt */
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
static volatile uint32_t s_park_seq; /* bumps each time the writer parks with
                                        both files closed (stop() waits on it) */
static bool s_hooked;                /* esp_restart shutdown handler in place */
#define DL_OPEN_FAILS_FAULT 3

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
    uint32_t want = (cfg->ring_len > DL_CAN_RING_MAX) ? DL_CAN_RING_MAX
                                                      : cfg->ring_len;

    if (s_frame_ring.cap != want)
    {
        /* a different live length re-bases the modular indices: anything
           carried over a warm reset in this ring is void (a settings
           change always came with a clean restart anyway) */
        s_frame_ring.cap = want;
        s_frame_ring.head = 0;
        s_frame_ring.tail = 0;
        s_frame_ring.fill = 0;
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

/* A dev_status fault latches to NVS — a flash write. The writer task's
 * stack is in PSRAM, and a task on a PSRAM stack must never write flash:
 * the cache is off during the write and the stack is gone with it
 * (cache_utils.c:126 assert — three panics on the bench, 2026-09-07).
 * Raise from a short-lived task on an internal stack instead. */
typedef struct
{
    char code[24];
    char detail[48];
} dl_fault_msg_t;

static void fault_task(void *arg)
{
    dl_fault_msg_t *m = arg;

    dev_status_manager_fault_raise(m->code, m->detail);
    heap_caps_free(m);
    vTaskDelete(NULL);
}

static void raise_fault_async(const char *code, const char *detail)
{
    dl_fault_msg_t *m = heap_caps_malloc(sizeof(*m),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (m == NULL)
    {
        return;
    }

    snprintf(m->code, sizeof(m->code), "%s", code);
    snprintf(m->detail, sizeof(m->detail), "%s", (detail != NULL) ? detail : "");

    if (xTaskCreate(fault_task, "dl_fault", 4096, m, 2, NULL) != pdPASS)
    {
        heap_caps_free(m);
        ESP_LOGW(TAG, "fault %s not latched (no task)", code);
    }
}

/* set-aside files (<name>.corrupt) of one stream; the oldest by name */
static uint32_t count_corrupt(const char *prefix, char *oldest, size_t cap)
{
    DIR *dir = opendir(DL_DIR);
    uint32_t n = 0;

    if (oldest != NULL && cap > 0)
    {
        oldest[0] = '\0';
    }

    if (dir == NULL)
    {
        return 0;
    }

    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL)
    {
        if (!dl_recover_is_corrupt_name(ent->d_name, prefix, NULL))
        {
            continue;
        }

        n++;

        if (oldest != NULL && cap > strlen(ent->d_name) &&
            (oldest[0] == '\0' || strcmp(ent->d_name, oldest) < 0))
        {
            strcpy(oldest, ent->d_name);
        }
    }

    closedir(dir);
    return n;
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

    bool resume = !force_new && scan.count > 0 &&
                  ext_matches(scan.newest, st->eng->ext);

    if (resume && st->eng->resume_max != 0)
    {
        struct stat sb;
        char p[80];

        snprintf(p, sizeof(p), DL_DIR "/%s", scan.newest);

        if (stat(p, &sb) == 0 && (uint64_t)sb.st_size > st->eng->resume_max)
        {
            ESP_LOGI(TAG, "%s is too big for the torn-tail check; starting "
                     "a new file", scan.newest);
            resume = false;
        }
    }

    if (resume)
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
    snprintf(st->last_try, sizeof(st->last_try), "%s", fname);

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
    st->corrupt_files = count_corrupt(st->prefix, NULL, 0);
    s_stats.corrupt = s_stream[ST_PARAM].corrupt_files +
                      s_stream[ST_CAN].corrupt_files;
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

/* keep at most DL_CORRUPT_KEEP set-aside files per stream */
static void prune_corrupt(dl_stream_t *st)
{
    char oldest[64];

    for (int guard = 0; guard < 8; guard++)
    {
        uint32_t n = count_corrupt(st->prefix, oldest, sizeof(oldest));

        st->corrupt_files = n;

        if (n <= DL_CORRUPT_KEEP || oldest[0] == '\0')
        {
            return;
        }

        char path[96];

        snprintf(path, sizeof(path), DL_DIR "/%s", oldest);

        if (unlink(path) != 0)
        {
            return;
        }

        ESP_LOGI(TAG, "set-aside cap: deleted %s", oldest);
    }
}

/* The file just failed as CORRUPT (ROBUSTNESS.md case 5): set it aside as
 * <name>.corrupt, drop its journal, latch a fault, copy what sqlite can
 * still read into a fresh file with the old name, and go on in a new
 * file. Never retry a bad file — that was the loop that dropped every
 * record. Writer task only. */
static void quarantine(dl_stream_t *st)
{
    char orig[48], bad[64], origpath[96], badpath[96], jpath[112];

    snprintf(orig, sizeof(orig), "%s",
             st->file[0] != '\0' ? st->file : st->last_try);
    close_current(st);

    if (orig[0] == '\0' || !dl_recover_corrupt_name(orig, bad, sizeof(bad)))
    {
        return;
    }

    snprintf(origpath, sizeof(origpath), DL_DIR "/%s", orig);
    snprintf(badpath, sizeof(badpath), DL_DIR "/%s", bad);
    snprintf(jpath, sizeof(jpath), "%s-journal", origpath);
    unlink(badpath); /* an older set-aside copy of the same name */

    if (rename(origpath, badpath) != 0)
    {
        ESP_LOGE(TAG, "%s: cannot set aside (errno %d); deleting it so "
                 "logging can go on", orig, errno);
        unlink(origpath);
        bad[0] = '\0';
    }

    unlink(jpath); /* a hot journal must never meet a new file of that name */
    raise_fault_async("logger_file_corrupt", orig);
    s_stats.corrupt++;
    dl_events_error("corrupt", (int)s_stats.corrupt);
    ESP_LOGE(TAG, "%s is corrupt: set aside as %s; logging continues in a "
             "fresh file", orig, bad[0] != '\0' ? bad : "(deleted)");
    prune_corrupt(st);

    if (bad[0] != '\0' && st->eng == &dl_engine_sqlite)
    {
        uint32_t rows = 0;

        if (dl_sq_salvage(badpath, origpath, st->frames, 120000, &rows) ==
                ESP_OK && rows > 0)
        {
            ESP_LOGW(TAG, "salvaged %lu row(s) from %s into %s",
                     (unsigned long)rows, bad, orig);
        }
    }

    if (open_current(st, true) != ESP_OK)
    {
        s_stats.errors++;
    }
}

/* open the stream's file; a corrupt one is set aside on the spot; repeated
 * failures latch a fault and slow the retries down (case 10) */
static bool ensure_open(dl_stream_t *st)
{
    if (st->eng->is_open(st->ctx))
    {
        return true;
    }

    if (open_current(st, false) == ESP_OK)
    {
        st->open_fails = 0;
        return true;
    }

    if (st->eng->corrupt != NULL && st->eng->corrupt(st->ctx))
    {
        quarantine(st);

        if (st->eng->is_open(st->ctx))
        {
            st->open_fails = 0;
            return true;
        }
    }

    s_stats.errors++;

    if (++st->open_fails == DL_OPEN_FAILS_FAULT)
    {
        ESP_LOGE(TAG, "%s stream: cannot open a file on the card; backing "
                 "off to 10 s retries", st->frames ? "CAN" : "params");
        raise_fault_async("logger_storage_error",
                          st->frames ? "can" : "params");
    }

    return false;
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
        s_stats.errors++;
        dl_events_error("write", (int)s_stats.errors);

        if (st->eng->corrupt != NULL && st->eng->corrupt(st->ctx))
        {
            quarantine(st); /* set aside + fresh file — never retry a bad file */
            return;
        }

        /* card yanked mid-write, disk full — drop the file handle and let
         * the mount/open path recover next lap */
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
            s_park_seq++; /* data_logger_stop() waits for this */
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
        bool backoff = false;

        for (int i = 0; i < ST_COUNT; i++)
        {
            dl_stream_t *st = &s_stream[i];

            if (st->active && !ensure_open(st))
            {
                all_open = false;
                backoff = backoff || st->open_fails >= DL_OPEN_FAILS_FAULT;
            }
        }

        if (!all_open)
        {
            s_stats.storage_ok = false;
            vTaskDelay(pdMS_TO_TICKS(backoff ? 10000 : 2000));
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

    uint32_t carried = ps_adopt();

    if (carried > 0)
    {
        s_stats.salvaged = carried;
        ESP_LOGW(TAG, "salvaged %lu record(s) queued before the last reset",
                 (unsigned long)carried);
    }

    dl_events_register();
    return dl_settings_register();
}

/* esp_restart() runs this before the reset — user restart, settings
 * apply, OTA, CLI, factory reset: the files are flushed and closed the
 * way sleep entry does it (ROBUSTNESS.md case 7). Panics never get here;
 * the PSRAM envelope covers those (case 6). */
static void dl_on_shutdown(void)
{
    if (s_run)
    {
        ESP_LOGI(TAG, "restart: flushing and closing the log files");
        data_logger_stop();
    }
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

    if (!s_hooked && esp_register_shutdown_handler(dl_on_shutdown) == ESP_OK)
    {
        s_hooked = true;
    }

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
    uint32_t seq = s_park_seq;

    s_run = false;
    s_stats.running = false;

    if (s_task == NULL)
    {
        return ESP_OK;
    }

    /* synchronous since 2026-09-07 (ROBUSTNESS.md cases 7/8): sleep entry
       unmounts the card right after this and a restart cuts the IO —
       wait for the writer to finish its batch and close both files */
    xTaskNotifyGive(s_task);

    for (int i = 0; i < 300 && s_park_seq == seq; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_park_seq == seq)
    {
        ESP_LOGW(TAG, "stop: the writer did not park within 3 s");
    }
    else
    {
        ESP_LOGI(TAG, "stopped: log files flushed and closed");
    }

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
        s_ps.reg_crc = reg_crc_calc(); /* the envelope must match at boot */
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
