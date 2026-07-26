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
 * @file log_sinks_file.c
 * @brief The file sink's writer: an INTERNAL-stack task (§2 corollary —
 *        the log_manager README's wear-safe recipe) draining the PSRAM
 *        file ring to /sd/devlog in coarse batches. Flush triggers:
 *        ring high-water, the flush_s period, or an explicit
 *        log_sinks_file_flush() (CLI `logsinks flush`).
 *
 *        Each flush is open -> write -> fflush+fsync -> CLOSE: no handle
 *        is held between flushes, so /api/fs/download of the active log
 *        always works (the data_logger FATFS-lock lesson) and a card
 *        yank between flushes costs nothing. One fsync per flush, never
 *        per line. Rotation folds into the open decision: append to the
 *        newest file while it's under file_max_kb, else start a new
 *        monotonic-epoch file and prune retention.
 *
 *        Mount-follow: parks while the card is absent; the PSRAM ring
 *        keeps absorbing (drop-oldest, counted).
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "external_storage.h"

#include "log_sinks.h"
#include "log_sinks_core.h"
#include "log_sinks_private.h"

static const char *TAG = "log_sinks";

static TaskHandle_t s_task;
static StaticTask_t s_tcb;      /* internal: FreeRTOS object */
#define LS_FILE_STACK 6144      /* internal heap, only when file_enabled:
                                   SD/VFS writes (§2); task never stops */
static StackType_t *s_stack;

static uint32_t s_rotations;
static volatile bool s_flush_req;
static uint8_t *s_drain;    /* LS_FILE_CHUNK, internal heap */

uint32_t ls_file_rotations(void)
{
    return s_rotations;
}

void ls_file_request_flush(void)
{
    s_flush_req = true;
    ls_file_notify();
}

void ls_file_notify(void)
{
    TaskHandle_t t = s_task;

    if (t != NULL)
    {
        xTaskNotifyGive(t);
    }
}

/** Scan LS_FILE_DIR for devlog_<epoch>.log files. Returns the count and
 *  fills @p epochs (up to @p max). */
static int scan_epochs(uint32_t *epochs, int max)
{
    DIR *dir = opendir(LS_FILE_DIR);
    int n = 0;

    if (dir == NULL)
    {
        return 0;
    }

    struct dirent *e;

    while ((e = readdir(dir)) != NULL && n < max)
    {
        unsigned epoch;

        if (sscanf(e->d_name, LS_FILE_PREFIX "_%u.log", &epoch) == 1)
        {
            epochs[n++] = epoch;
        }
    }

    closedir(dir);
    return n;
}

static void path_for(char *out, size_t len, uint32_t epoch)
{
    snprintf(out, len, LS_FILE_DIR "/" LS_FILE_PREFIX "_%lu.log",
             (unsigned long)epoch);
}

static void prune(int keep)
{
    uint32_t epochs[LS_FILE_KEEP_MAX * 2];
    int n = scan_epochs(epochs, LS_FILE_KEEP_MAX * 2);
    int victims = ls_rotate_victims(epochs, n, keep);

    for (int i = 0; i < victims; i++)
    {
        char path[64];

        path_for(path, sizeof(path), epochs[i]);
        unlink(path);
    }
}

/** Open the append target for this flush: the newest file while it's
 *  under the size cap, else a fresh monotonic-epoch file (+ prune). */
static FILE *open_current(void)
{
    const log_sinks_config_t *cfg = ls_settings_config();
    uint32_t epochs[LS_FILE_KEEP_MAX * 2];
    int n = scan_epochs(epochs, LS_FILE_KEEP_MAX * 2);
    uint32_t newest = 0;
    uint32_t open_epoch = 0;
    char path[64];

    mkdir(LS_FILE_DIR, 0775);

    for (int i = 0; i < n; i++)
    {
        if (epochs[i] > newest)
        {
            newest = epochs[i];
        }
    }

    if (newest > 0)
    {
        struct stat st;

        path_for(path, sizeof(path), newest);

        if (stat(path, &st) == 0 &&
            (uint32_t)st.st_size < (uint32_t)cfg->file_max_kb * 1024u)
        {
            open_epoch = newest; /* resume the newest under-cap file */
        }
    }

    if (open_epoch == 0)
    {
        open_epoch = ls_rotate_next_epoch((uint32_t)time(NULL), newest);
        path_for(path, sizeof(path), open_epoch);

        if (newest > 0)
        {
            s_rotations++; /* the previous file hit the cap */
        }

        FILE *f = fopen(path, "a");

        if (f != NULL)
        {
            /* prune AFTER the new file exists so retention counts it —
             * pruning first leaves file_keep+1 files behind */
            prune(cfg->file_keep);
        }

        return f;
    }

    return fopen(path, "a");
}

/** One flush: drain the ring in LS_FILE_CHUNK units into the current
 *  file, one fflush+fsync, close. */
static void flush_ring(void)
{
    FILE *f = NULL;

    for (;;)
    {
        uint32_t n = ls_file_ring_pop_batch(s_drain, LS_FILE_CHUNK, NULL);

        if (n == 0)
        {
            break;
        }

        if (f == NULL)
        {
            f = open_current();

            if (f == NULL)
            {
                ESP_LOGD(TAG, "file sink: open failed (card gone?)");
                return; /* this chunk is lost; the ring keeps the rest */
            }
        }

        if (fwrite(s_drain, 1, n, f) != n)
        {
            break; /* write error: fsync what landed, retry next lap */
        }
    }

    if (f != NULL)
    {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

static void writer_task(void *arg)
{
    const log_sinks_config_t *cfg = ls_settings_config();
    TickType_t last_flush = xTaskGetTickCount();

    (void)arg;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        if (!external_storage_is_mounted())
        {
            continue; /* park; the PSRAM ring keeps absorbing */
        }

        TickType_t now = xTaskGetTickCount();
        bool period = (now - last_flush) >=
                      pdMS_TO_TICKS((uint32_t)cfg->file_flush_s * 1000u);
        bool hiwater = ls_file_ring_used() >= LS_FILE_HIWATER;

        if (s_flush_req || hiwater || (period && ls_file_ring_used() > 0))
        {
            s_flush_req = false;
            flush_ring();
            last_flush = now;
        }
    }
}

esp_err_t ls_file_start(void)
{
    if (s_task != NULL)
    {
        return ESP_OK;
    }

    /* internal + DMA-capable: SD/FATFS may DMA from this buffer */
    s_drain = heap_caps_malloc(LS_FILE_CHUNK,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_stack = heap_caps_malloc(LS_FILE_STACK * sizeof(StackType_t),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (s_drain == NULL || s_stack == NULL)
    {
        heap_caps_free(s_drain);
        heap_caps_free(s_stack);
        s_drain = NULL;
        s_stack = NULL;
        ESP_LOGE(TAG, "file sink buffers: no internal RAM");
        return ESP_ERR_NO_MEM;
    }

    s_task = xTaskCreateStatic(writer_task, "log_sinks_fw", LS_FILE_STACK,
                               NULL, 2, s_stack, &s_tcb);
    return (s_task != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t log_sinks_file_flush(void)
{
    if (s_task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ls_file_request_flush();
    return ESP_OK;
}
