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
 * @file es_close.c
 * @brief The close guard of the card's FAT volume (2026-11-14): a file
 *        whose close could not flush is released all the same.
 *
 * FatFs's f_close() flushes first (f_sync) and returns as soon as that
 * fails, BEFORE it gives the file's lock back (FF_FS_LOCK, ff.c: the
 * dec_share() call sits behind the `res == FR_OK`). IDF's VFS then wipes
 * its own slot whatever the result (vfs_fat_close, file_cleanup). The file
 * object is gone, the lock is not: until the card is mounted again nobody
 * can open that file, for writing or for reading (FR_LOCKED, errno 13).
 *
 * How a flush fails on a healthy card: the SD driver needs a DMA-capable
 * bounce buffer of one sector for every transfer from a buffer that is not
 * DMA-capable or not aligned (FatFs keeps its sector windows in PSRAM), and
 * takes it from the internal heap per transfer. On a unit short of internal
 * RAM that allocation fails (`sdmmc_cmd: allocate_dma_buf: not enough mem`),
 * the write fails, the writer closes its file, and the close fails the same
 * way. Seen on the bench with the internal heap squeezed to zero: the
 * logger's database could not be opened again with all the memory back
 * (`unable to open database file` every 10 s), and its download answered
 * 409 `file in use` with no file open anywhere. The field report of
 * 2026-11-13 reads the same: `logger_storage_error`, no file name on the
 * Logger page, the newest log file not downloadable, on a good card.
 *
 * The guard wraps f_close() at link time (the lwip_guard / ip4_routes
 * pattern: IDF has no hook here). When the first close fails and FatFs
 * still knows the object, it drops the object's "modified" and "dirty"
 * status bits and closes again: with nothing left to write FatFs goes
 * straight to the release. What the failed flush did not bring to the card
 * is lost, as after a power cut at that moment (the directory entry keeps
 * the size of the last successful sync; the logger's resume cuts a torn
 * tail). The caller still gets the first close's error.
 */
#include <stdint.h>

#include "esp_log.h"
#include "ff.h"

#include "external_storage_private.h"

static const char *TAG = "external_storage";

static uint32_t s_forced;

uint32_t es_close_forced(void)
{
    return s_forced;
}

FRESULT __real_f_close(FIL *fp);

FRESULT __wrap_f_close(FIL *fp)
{
    FRESULT res = __real_f_close(fp);

    if (fp == NULL)
    {
        return res;
    }

    uint8_t flag = fp->flag;

    if (!es_close_retry(res == FR_OK, fp->obj.fs != NULL, &flag))
    {
        return res;
    }

    fp->flag = flag;

    FRESULT again = __real_f_close(fp);

    s_forced++;
    ESP_LOGW(TAG, "a file's close could not flush (FatFs result %d): its "
             "lock is %s; what was not on the card yet is lost (%lu so far)",
             (int)res, (again == FR_OK) ? "released" : "still held",
             (unsigned long)s_forced);
    return res;
}
