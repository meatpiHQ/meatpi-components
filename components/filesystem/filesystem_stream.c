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
 * @file filesystem_stream.c
 * @brief Streaming atomic writes for big files (downloads, streamed
 *        uploads): open a TEMP sibling, write chunk by chunk, commit =
 *        fsync + rename, an interrupted stream never leaves a torn
 *        file. ONE stream at a time; chunk writes serialize with other
 *        fs operations per chunk (long streams don't starve them).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "flash_jobs.h"

#include "filesystem.h"
#include "filesystem_private.h"

static const char *TAG = "filesystem";

struct filesystem_wstream
{
    FILE *f;
    char  final_path[FS_PATH_MAX];
    char  tmp_path[FS_PATH_MAX];
    bool  used;
    bool  flash; /* a /data stream: its writes need an internal stack */
};

static struct filesystem_wstream s_wstream; /* ONE stream at a time */

/* The stack rule for the streams (2026-10-31): a /data stream's open,
   chunks, commit and abort reach the flash with the cache off, so each of
   the four runs through fs_run_internal (the flash_jobs worker) when the
   caller's stack is in PSRAM: the async pipe's writer task itself, the
   HTTP server task. An /sd stream runs on the caller's stack. */

typedef struct
{
    const char            *path;
    filesystem_wstream_t **out;
} wopen_args_t;

static esp_err_t wopen_job(void *p)
{
    wopen_args_t *a = p;

    return filesystem_write_open(a->path, a->out);
}

esp_err_t filesystem_write_open(const char *path,
                                filesystem_wstream_t **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    fs_backend_t backend;
    esp_err_t err = fs_check_path(path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        wopen_args_t a = { path, out };

        return fs_run_internal(wopen_job, &a);
    }

    fs_lock();

    if (s_wstream.used)
    {
        fs_unlock();
        return ESP_ERR_INVALID_STATE; /* one stream at a time */
    }

    err = fs_path_temp_name(path, s_wstream.tmp_path,
                            sizeof(s_wstream.tmp_path));

    char parent[FS_PATH_MAX];

    if (err == ESP_OK &&
        fs_path_parent(path, parent, sizeof(parent)) == ESP_OK)
    {
        err = fs_mkdirs_in_lock(parent);
    }

    if (err == ESP_OK)
    {
        s_wstream.f = fopen(s_wstream.tmp_path, "wb");

        if (s_wstream.f == NULL)
        {
            ESP_LOGE(TAG, "stream open '%s' failed: errno %d",
                     s_wstream.tmp_path, errno);
            err = fs_errno_to_esp(errno);
        }
    }

    if (err == ESP_OK)
    {
        snprintf(s_wstream.final_path, sizeof(s_wstream.final_path), "%s",
                 path);
        s_wstream.used = true;
        s_wstream.flash = (backend == FS_BACKEND_INTERNAL);
        *out = &s_wstream;
    }

    fs_unlock();
    return err;
}

typedef struct
{
    filesystem_wstream_t *ws;
    const void           *data;
    size_t                len;
} wchunk_args_t;

static esp_err_t wchunk_job(void *p)
{
    wchunk_args_t *a = p;

    return filesystem_write_chunk(a->ws, a->data, a->len);
}

esp_err_t filesystem_write_chunk(filesystem_wstream_t *ws,
                                 const void *data, size_t len)
{
    if (ws == NULL || !ws->used || (data == NULL && len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ws->flash && !fs_on_internal_stack())
    {
        wchunk_args_t a = { ws, data, len };

        return fs_run_internal(wchunk_job, &a);
    }

    const uint8_t *src = data;
    esp_err_t err = ESP_OK;

    fs_lock(); /* per chunk: long streams don't starve other fs users */

    size_t scratch_size = 0;
    uint8_t *scratch = fs_scratch(&scratch_size);

    while (len > 0 && err == ESP_OK)
    {
        size_t chunk = (len > scratch_size) ? scratch_size : len;

        memcpy(scratch, src, chunk); /* PSRAM -> internal, cache on */

        if (fwrite(scratch, 1, chunk, ws->f) != chunk)
        {
            ESP_LOGE(TAG, "stream write '%s' failed: errno %d",
                     ws->final_path, errno);
            err = fs_errno_to_esp(errno);
        }

        src += chunk;
        len -= chunk;
    }

    fs_unlock();
    return err;
}

static esp_err_t wcommit_job(void *p)
{
    return filesystem_write_commit((filesystem_wstream_t *)p);
}

esp_err_t filesystem_write_commit(filesystem_wstream_t *ws)
{
    if (ws == NULL || !ws->used)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ws->flash && !fs_on_internal_stack())
    {
        return fs_run_internal(wcommit_job, ws);
    }

    esp_err_t err = ESP_OK;

    fs_lock();
    fflush(ws->f);
    fsync(fileno(ws->f)); /* force to media before the rename */

    if (fclose(ws->f) != 0 ||
        fs_rename_replace(ws->tmp_path, ws->final_path) != 0)
    {
        ESP_LOGE(TAG, "stream commit '%s' failed: errno %d",
                 ws->final_path, errno);
        unlink(ws->tmp_path);
        err = ESP_FAIL;
    }

    ws->f = NULL;
    ws->used = false;
    fs_unlock();
    return err;
}

static esp_err_t wabort_job(void *p)
{
    filesystem_write_abort((filesystem_wstream_t *)p);
    return ESP_OK;
}

void filesystem_write_abort(filesystem_wstream_t *ws)
{
    if (ws == NULL || !ws->used)
    {
        return;
    }

    if (ws->flash && !fs_on_internal_stack())
    {
        (void)fs_run_internal(wabort_job, ws);
        return;
    }

    fs_lock();
    fclose(ws->f);
    unlink(ws->tmp_path);
    ws->f = NULL;
    ws->used = false;
    fs_unlock();
}

/* ---- the async pipe ---------------------------------------------------------- */

#define FS_ASYNC_SLOT_SIZE (32 * 1024)

typedef struct
{
    uint8_t data[FS_ASYNC_SLOT_SIZE];
    size_t  len;
} fs_async_slot_t;

static fs_async_slot_t s_slot[2] EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t s_slot_filled;      /* producer -> writer      */
static StaticSemaphore_t s_slot_filled_buf;  /* internal: FreeRTOS obj  */
static SemaphoreHandle_t s_slot_free;        /* writer -> producer      */
static StaticSemaphore_t s_slot_free_buf;    /* internal: FreeRTOS obj  */
static SemaphoreHandle_t s_async_done;       /* writer: drained         */
static StaticSemaphore_t s_async_done_buf;   /* internal: FreeRTOS obj  */
static TaskHandle_t s_writer_task;
static StaticTask_t s_writer_tcb;            /* internal: FreeRTOS obj  */
/* PSRAM since 2026-10-31: the writer only drains the pipe; each chunk's
 * flash write goes through filesystem_write_chunk(), which hands a
 * PSRAM-stack caller to the flash_jobs worker like every other /data
 * operation. 5 KB internal before that (the marshalled jobs of PSRAM-stack
 * callers ran here from 2026-10-13 to 2026-10-31). */
static StackType_t s_writer_stack[5120] EXT_RAM_BSS_ATTR;
static filesystem_wstream_t *volatile s_async_ws;
static volatile esp_err_t s_async_err;
static int s_prod_idx; /* producer/writer stay in lockstep forever      */

static void writer_task(void *arg)
{
    int idx = 0;

    (void)arg;

    while (true)
    {
        xSemaphoreTake(s_slot_filled, portMAX_DELAY);

        fs_async_slot_t *slot = &s_slot[idx];

        idx ^= 1;

        if (slot->len > 0)
        {
            if (s_async_err == ESP_OK)
            {
                s_async_err = filesystem_write_chunk(s_async_ws,
                                                     slot->data,
                                                     slot->len);
            }

            xSemaphoreGive(s_slot_free);
        }
        else
        {
            xSemaphoreGive(s_slot_free);
            xSemaphoreGive(s_async_done); /* len 0 sentinel = drained */
        }
    }
}

esp_err_t fs_stream_service_init(void)
{
    s_slot_filled = xSemaphoreCreateCountingStatic(2, 0,
                                                   &s_slot_filled_buf);
    s_slot_free = xSemaphoreCreateCountingStatic(2, 2, &s_slot_free_buf);
    s_async_done = xSemaphoreCreateBinaryStatic(&s_async_done_buf);
    s_writer_task = xTaskCreateStatic(writer_task, "fs_writer",
                                      sizeof(s_writer_stack) /
                                          sizeof(s_writer_stack[0]),
                                      NULL, 5, s_writer_stack,
                                      &s_writer_tcb);
    return (s_writer_task != NULL) ? ESP_OK : ESP_FAIL;
}

/* ---- the internal-stack service: the shared flash_jobs worker ---------------- */

bool fs_on_internal_stack(void)
{
    return flash_jobs_on_internal_stack();
}

esp_err_t fs_run_internal(fs_job_fn_t fn, void *arg)
{
    if (fn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* in line on an internal stack (a job inside a job included), on the
       worker otherwise; the worker is the only internal stack the
       filesystem needs since 2026-10-31 */
    return flash_jobs_run(fn, arg);
}

esp_err_t filesystem_write_async_begin(filesystem_wstream_t *ws)
{
    if (ws == NULL || !ws->used || s_writer_task == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_async_ws != NULL)
    {
        return ESP_ERR_INVALID_STATE; /* one pipe at a time */
    }

    s_async_err = ESP_OK;
    s_async_ws = ws;
    return ESP_OK;
}

esp_err_t filesystem_write_async_chunk(filesystem_wstream_t *ws,
                                       const void *data, size_t len)
{
    if (ws == NULL || ws != s_async_ws ||
        (data == NULL && len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *src = data;

    while (len > 0)
    {
        size_t piece = (len > FS_ASYNC_SLOT_SIZE) ? FS_ASYNC_SLOT_SIZE
                                                  : len;

        xSemaphoreTake(s_slot_free, portMAX_DELAY);
        memcpy(s_slot[s_prod_idx].data, src, piece);
        s_slot[s_prod_idx].len = piece;
        s_prod_idx ^= 1;
        xSemaphoreGive(s_slot_filled);
        src += piece;
        len -= piece;
    }

    return s_async_err; /* surface write errors early */
}

esp_err_t filesystem_write_async_end(filesystem_wstream_t *ws,
                                     bool commit)
{
    if (ws == NULL || ws != s_async_ws)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* len-0 sentinel, then wait until the writer has drained */
    xSemaphoreTake(s_slot_free, portMAX_DELAY);
    s_slot[s_prod_idx].len = 0;
    s_prod_idx ^= 1;
    xSemaphoreGive(s_slot_filled);
    xSemaphoreTake(s_async_done, portMAX_DELAY);

    esp_err_t err = s_async_err;

    s_async_ws = NULL;

    if (commit && err == ESP_OK)
    {
        return filesystem_write_commit(ws);
    }

    filesystem_write_abort(ws);
    return err; /* caller-requested abort with no write error = ESP_OK */
}
