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
 * @file data_logger_db.c
 * @brief The sqlite storage engine. Called from the writer task ONLY —
 *        the vendored port is compiled SQLITE_THREADSAFE=0
 *        (sqlite3/PROVENANCE.md), so the single-toucher rule is a hard
 *        requirement, not style. Takes a dl_sq_ctx_t since 2026-07-09
 *        (two streams can both pick sqlite = two open .db files).
 *
 * Pragma hygiene per BENCHMARKS.md (the 5.6× fix): everything set ONCE
 * at open — journal_mode=MEMORY (WAL is compiled out and would fail
 * silently; DELETE would churn a journal file on the card per commit),
 * temp_store=MEMORY — plus forever-prepared INSERTs.
 *
 * Schema: params(id, source, name UNIQUE) + records(ts, param_id,
 * value REAL) + frames(ts, id, ext, rtr, dlc, data BLOB), ts-indexed.
 * NOTE the measured ~700 rows/s ceiling — sqlite for the CAN stream is
 * a filtered/slow-bus choice, not a busy-bus one (README).
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sqlite3.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

/* ---- PSRAM allocator for sqlite -------------------------------------------
   Internal RAM is the scarce resource on this firmware (~25 KB free at
   steady state); sqlite's page cache and parser must not compete with
   the WiFi/DMA pools, so all its heap goes to PSRAM. */

static void *sq_mem_malloc(int n)
{
    return heap_caps_malloc((size_t)n,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void sq_mem_free(void *p)
{
    heap_caps_free(p);
}

static void *sq_mem_realloc(void *p, int n)
{
    return heap_caps_realloc(p, (size_t)n,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static int sq_mem_size(void *p)
{
    return (p != NULL) ? (int)heap_caps_get_allocated_size(p) : 0;
}

static int sq_mem_roundup(int n)
{
    return (n + 7) & ~7;
}

static int sq_mem_init(void *arg)
{
    (void)arg;
    return SQLITE_OK;
}

static void sq_mem_shutdown(void *arg)
{
    (void)arg;
}

static esp_err_t dl_exec(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;

    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "sql '%s': %s", sql,
                 (errmsg != NULL) ? errmsg : "?");
        sqlite3_free(errmsg);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void sq_close(void *ctx)
{
    dl_sq_ctx_t *c = ctx;

    if (c->ins_rec != NULL)
    {
        sqlite3_finalize((sqlite3_stmt *)c->ins_rec);
        c->ins_rec = NULL;
    }

    if (c->ins_frame != NULL)
    {
        sqlite3_finalize((sqlite3_stmt *)c->ins_frame);
        c->ins_frame = NULL;
    }

    if (c->db != NULL)
    {
        sqlite3_close((sqlite3 *)c->db);
        c->db = NULL;
    }
}

static esp_err_t sq_open(void *ctx, const char *path, bool frames)
{
    dl_sq_ctx_t *c = ctx;

    (void)frames; /* both tables are always created (cheap) */

    /* the port defines SQLITE_OMIT_AUTOINIT: without this, the first
     * malloc jumps through an uninitialized xMalloc pointer */
    static bool s_lib_ready;

    if (c->db != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lib_ready)
    {
        static const sqlite3_mem_methods PSRAM_MEM =
        {
            .xMalloc = sq_mem_malloc,
            .xFree = sq_mem_free,
            .xRealloc = sq_mem_realloc,
            .xSize = sq_mem_size,
            .xRoundup = sq_mem_roundup,
            .xInit = sq_mem_init,
            .xShutdown = sq_mem_shutdown,
        };

        (void)sqlite3_config(SQLITE_CONFIG_MALLOC, &PSRAM_MEM);

        if (sqlite3_initialize() != SQLITE_OK)
        {
            ESP_LOGE(TAG, "sqlite3_initialize failed");
            return ESP_FAIL;
        }

        s_lib_ready = true;
    }

    sqlite3 *db = NULL;

    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "open %s: %s", path,
                 (db != NULL) ? sqlite3_errmsg(db) : "?");
        c->db = db;
        sq_close(c);
        return ESP_FAIL;
    }

    c->db = db;

    if (dl_exec(db, "PRAGMA journal_mode=MEMORY;") != ESP_OK ||
        dl_exec(db, "PRAGMA temp_store=MEMORY;") != ESP_OK ||
        dl_exec(db, "CREATE TABLE IF NOT EXISTS params ("
                "id INTEGER PRIMARY KEY, source TEXT, name TEXT, "
                "UNIQUE(source, name));") != ESP_OK ||
        dl_exec(db, "CREATE TABLE IF NOT EXISTS records ("
                "ts INTEGER, param_id INTEGER, value REAL);") != ESP_OK ||
        dl_exec(db, "CREATE INDEX IF NOT EXISTS records_ts "
                "ON records(ts);") != ESP_OK ||
        dl_exec(db, "CREATE TABLE IF NOT EXISTS frames ("
                "ts INTEGER, id INTEGER, ext INTEGER, rtr INTEGER, "
                "dlc INTEGER, data BLOB);") != ESP_OK ||
        dl_exec(db, "CREATE INDEX IF NOT EXISTS frames_ts "
                "ON frames(ts);") != ESP_OK)
    {
        sq_close(c);
        return ESP_FAIL;
    }

    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO records (ts, param_id, value) "
                           "VALUES (?, ?, ?);", -1, &st, NULL) !=
        SQLITE_OK)
    {
        ESP_LOGE(TAG, "prepare records: %s", sqlite3_errmsg(db));
        sq_close(c);
        return ESP_FAIL;
    }

    c->ins_rec = st;
    st = NULL;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO frames "
                           "(ts, id, ext, rtr, dlc, data) "
                           "VALUES (?, ?, ?, ?, ?, ?);", -1, &st,
                           NULL) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "prepare frames: %s", sqlite3_errmsg(db));
        sq_close(c);
        return ESP_FAIL;
    }

    c->ins_frame = st;

    /* resuming an existing file: seed the size estimates */
    c->rows = 0;
    c->frame_rows = 0;
    st = NULL;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM records;", -1,
                           &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
    {
        c->rows = (uint64_t)sqlite3_column_int64(st, 0);
    }

    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM frames;", -1,
                           &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
    {
        c->frame_rows = (uint64_t)sqlite3_column_int64(st, 0);
    }

    sqlite3_finalize(st);
    return ESP_OK;
}

static bool sq_is_open(void *ctx)
{
    return ((dl_sq_ctx_t *)ctx)->db != NULL;
}

/* intern source.name into params, returns db id (>0) or -1 */
static int sq_intern(sqlite3 *db, const char *source, const char *name)
{
    sqlite3_stmt *st = NULL;
    int id = -1;

    if (sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO params (source, name) "
                           "VALUES (?, ?);", -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, source, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }

    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
                           "SELECT id FROM params WHERE source = ? "
                           "AND name = ?;", -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, source, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);

        if (sqlite3_step(st) == SQLITE_ROW)
        {
            id = sqlite3_column_int(st, 0);
        }
    }

    sqlite3_finalize(st);

    if (id < 0)
    {
        ESP_LOGE(TAG, "intern %s.%s failed: %s", source, name,
                 sqlite3_errmsg(db));
    }

    return id;
}

static esp_err_t sq_begin(void *ctx)
{
    return dl_exec((sqlite3 *)((dl_sq_ctx_t *)ctx)->db, "BEGIN;");
}

static esp_err_t sq_write(void *ctx, const dl_record_t *rec,
                          dl_param_entry_t *p)
{
    dl_sq_ctx_t *c = ctx;

    if (rec->kind == DL_REC_FRAME)
    {
        sqlite3_stmt *st = c->ins_frame;

        if (st == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }

        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, rec->ts_ms);
        sqlite3_bind_int64(st, 2, (int64_t)rec->u.f.id);
        sqlite3_bind_int(st, 3, (rec->u.f.flags & DL_FRAME_EXT) ? 1 : 0);
        sqlite3_bind_int(st, 4, (rec->u.f.flags & DL_FRAME_RTR) ? 1 : 0);
        sqlite3_bind_int(st, 5, rec->u.f.dlc);
        sqlite3_bind_blob(st, 6, rec->u.f.data, rec->u.f.dlc,
                          SQLITE_STATIC);

        if (sqlite3_step(st) != SQLITE_DONE)
        {
            ESP_LOGE(TAG, "frame insert: %s",
                     sqlite3_errmsg((sqlite3 *)c->db));
            return ESP_FAIL;
        }

        c->frame_rows++;
        return ESP_OK;
    }

    sqlite3_stmt *st = c->ins_rec;

    if (st == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (p->db_id <= 0)
    {
        p->db_id = sq_intern((sqlite3 *)c->db, p->source, p->name);

        if (p->db_id <= 0)
        {
            return ESP_FAIL;
        }
    }

    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, rec->ts_ms);
    sqlite3_bind_int(st, 2, p->db_id);
    sqlite3_bind_double(st, 3, rec->u.p.value);

    if (sqlite3_step(st) != SQLITE_DONE)
    {
        ESP_LOGE(TAG, "insert: %s", sqlite3_errmsg((sqlite3 *)c->db));
        return ESP_FAIL;
    }

    c->rows++;
    return ESP_OK;
}

static esp_err_t sq_commit(void *ctx)
{
    return dl_exec((sqlite3 *)((dl_sq_ctx_t *)ctx)->db, "COMMIT;");
}

static uint64_t sq_bytes(void *ctx)
{
    dl_sq_ctx_t *c = ctx;

    return c->rows * DL_BYTES_PER_ROW +
           c->frame_rows * DL_BYTES_PER_FRAME;
}

const dl_engine_t dl_engine_sqlite =
{
    .ext = ".db",
    .open = sq_open,
    .close = sq_close,
    .is_open = sq_is_open,
    .begin = sq_begin,
    .write = sq_write,
    .commit = sq_commit,
    .bytes = sq_bytes,
};
