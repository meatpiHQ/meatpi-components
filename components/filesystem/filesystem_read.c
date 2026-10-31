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
 * @file filesystem_read.c
 * @brief The reading operations of the filesystem component: a whole file
 *        into a buffer, size, existence, a directory listing, a raw stdio
 *        handle and the capacity of a backend (split out of filesystem.c
 *        2026-10-13). The same stack rule as the writes: a /data operation
 *        from a PSRAM-stack task runs on the flash worker (LittleFS reads go
 *        through esp_partition_read, which disables the flash cache too).
 *        A stream from filesystem_open() is the caller's, but its open,
 *        reads, stat and close go through the same rule when the caller
 *        uses filesystem_fread() and its siblings (2026-10-31).
 */
#include "filesystem.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_vfs_fat.h" /* /sd capacity (backend mounted by external_storage) */
#include "sdkconfig.h"

#include "filesystem_private.h"

/* ---- read ------------------------------------------------------------------- */

typedef struct
{
    const char *path;
    void       *buf;
    size_t      buf_len;
    size_t     *out_len;
} read_args_t;

static esp_err_t read_job(void *p)
{
    read_args_t *a = p;

    return filesystem_read(a->path, a->buf, a->buf_len, a->out_len);
}

esp_err_t filesystem_read(const char *path, void *buf, size_t buf_len,
                          size_t *out_len)
{
    fs_backend_t backend;
    esp_err_t    err = fs_check_path(path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (buf == NULL && buf_len > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        read_args_t a = { path, buf, buf_len, out_len };

        return fs_run_internal(read_job, &a);
    }

    fs_lock();

    FILE *f = fopen(path, "rb");

    if (f == NULL)
    {
        fs_unlock();
        return fs_errno_to_esp(errno);
    }

    struct stat st;

    if (fstat(fileno(f), &st) != 0)
    {
        fclose(f);
        fs_unlock();
        return ESP_FAIL;
    }

    size_t size = (size_t)st.st_size;

    if (out_len != NULL)
    {
        *out_len = size;
    }

    if (size > buf_len)
    {
        fclose(f);
        fs_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    size_t   scratch_size = 0;
    uint8_t *scratch = fs_scratch(&scratch_size);
    uint8_t *dst = buf;
    size_t   left = size;

    while (left > 0)
    {
        size_t chunk = (left > scratch_size) ? scratch_size : left;

        if (fread(scratch, 1, chunk, f) != chunk)
        {
            fclose(f);
            fs_unlock();
            return ESP_FAIL;
        }

        memcpy(dst, scratch, chunk); /* internal -> caller (maybe PSRAM) */
        dst += chunk;
        left -= chunk;
    }

    fclose(f);
    fs_unlock();
    return ESP_OK;
}

/* ---- size / exists ------------------------------------------------------------ */

typedef struct
{
    const char *path;
    size_t     *out_size;
} size_args_t;

static esp_err_t size_job(void *p)
{
    size_args_t *a = p;

    return filesystem_size(a->path, a->out_size);
}

esp_err_t filesystem_size(const char *path, size_t *out_size)
{
    fs_backend_t backend;
    esp_err_t    err = fs_check_path(path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (out_size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        size_args_t a = { path, out_size };

        return fs_run_internal(size_job, &a);
    }

    fs_lock();

    struct stat st;
    int rc = stat(path, &st);

    fs_unlock();

    if (rc != 0)
    {
        return fs_errno_to_esp(errno);
    }

    *out_size = (size_t)st.st_size;
    return ESP_OK;
}

bool filesystem_exists(const char *path)
{
    size_t size = 0;

    /* the size query carries the stack rule; NOT_FOUND and every other
       failure read as absent, as before */
    return filesystem_size(path, &size) == ESP_OK;
}

/* ---- list ----------------------------------------------------------------- */

typedef struct
{
    const char           *dir_path;
    filesystem_list_cb_t  cb;
    void                 *ctx;
} list_args_t;

static esp_err_t list_job(void *p)
{
    list_args_t *a = p;

    return filesystem_list(a->dir_path, a->cb, a->ctx);
}

esp_err_t filesystem_list(const char *dir_path, filesystem_list_cb_t cb,
                          void *ctx)
{
    fs_backend_t backend;
    esp_err_t    err = fs_check_path(dir_path, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        /* the callback then runs on the writer task's stack: keep it small */
        list_args_t a = { dir_path, cb, ctx };

        return fs_run_internal(list_job, &a);
    }

    fs_lock();

    DIR *dir = opendir(dir_path);

    if (dir == NULL)
    {
        fs_unlock();
        return fs_errno_to_esp(errno);
    }

    struct dirent *ent;

    err = ESP_OK;

    while ((ent = readdir(dir)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }

        char full[FS_PATH_MAX];

        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name)
            >= (int)sizeof(full))
        {
            continue; /* name doesn't fit our namespace; skip, don't fail */
        }

        struct stat st;
        bool   is_dir = false;
        size_t size = 0;

        if (stat(full, &st) == 0)
        {
            is_dir = S_ISDIR(st.st_mode);
            size = is_dir ? 0 : (size_t)st.st_size;
        }

        err = cb(ent->d_name, is_dir, size, ctx);

        if (err != ESP_OK)
        {
            break;
        }
    }

    closedir(dir);
    fs_unlock();
    return err;
}

/* ---- a raw handle: the caller's stream, the component's stack rule ------------
   The open of a /data stream reads the flash (the directory walk) and so
   runs on the worker for a PSRAM-stack caller; the stream itself is the
   caller's. Its reads, stat and close carry the rule when the caller uses
   the helpers below: those do not know the backend of a handle, so they
   marshal every call made from a PSRAM stack (an /sd stream from the HTTP
   server task runs on the worker too, which is harmless and serializes
   SD streaming behind the flash jobs for the duration of a chunk). */

typedef struct
{
    const char *path;
    const char *mode;
    FILE       *f;
} open_args_t;

static esp_err_t open_job(void *p)
{
    open_args_t *a = p;

    a->f = fopen(a->path, a->mode);
    return (a->f != NULL) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

FILE *filesystem_open(const char *path, const char *mode)
{
    fs_backend_t backend;

    if (mode == NULL || fs_check_path(path, &backend) != ESP_OK)
    {
        return NULL;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        open_args_t a = { path, mode, NULL };

        (void)fs_run_internal(open_job, &a);
        return a.f;
    }

    return fopen(path, mode);
}

typedef struct
{
    void   *buf;
    size_t  len;
    FILE   *f;
    size_t  got;
} fread_args_t;

static esp_err_t fread_job(void *p)
{
    fread_args_t *a = p;

    a->got = fread(a->buf, 1, a->len, a->f);
    return ESP_OK;
}

size_t filesystem_fread(void *buf, size_t len, FILE *f)
{
    if (f == NULL || (buf == NULL && len > 0))
    {
        return 0;
    }

    if (!fs_on_internal_stack())
    {
        fread_args_t a = { buf, len, f, 0 };

        (void)fs_run_internal(fread_job, &a);
        return a.got;
    }

    return fread(buf, 1, len, f);
}

typedef struct
{
    FILE        *f;
    struct stat *st;
} fstat_args_t;

static esp_err_t fstat_job(void *p)
{
    fstat_args_t *a = p;

    return (fstat(fileno(a->f), a->st) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t filesystem_fstat(FILE *f, struct stat *st)
{
    if (f == NULL || st == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fs_on_internal_stack())
    {
        fstat_args_t a = { f, st };

        return fs_run_internal(fstat_job, &a);
    }

    return (fstat(fileno(f), st) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t fclose_job(void *p)
{
    return (fclose((FILE *)p) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t filesystem_fclose(FILE *f)
{
    if (f == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fs_on_internal_stack())
    {
        return fs_run_internal(fclose_job, f);
    }

    return (fclose(f) == 0) ? ESP_OK : ESP_FAIL;
}

/* ---- capacity ---------------------------------------------------------------- */

typedef struct
{
    const char *prefix;
    uint64_t   *out_total;
    uint64_t   *out_used;
} info_args_t;

static esp_err_t info_job(void *p)
{
    info_args_t *a = p;

    return filesystem_info(a->prefix, a->out_total, a->out_used);
}

esp_err_t filesystem_info(const char *prefix, uint64_t *out_total,
                          uint64_t *out_used)
{
    fs_backend_t backend;
    esp_err_t    err = fs_check_path(prefix, &backend);

    if (err != ESP_OK)
    {
        return err;
    }

    if (backend == FS_BACKEND_INTERNAL && !fs_on_internal_stack())
    {
        info_args_t a = { prefix, out_total, out_used };

        return fs_run_internal(info_job, &a);
    }

    uint64_t total = 0;
    uint64_t used = 0;

    if (backend == FS_BACKEND_INTERNAL)
    {
        size_t lfs_total = 0;
        size_t lfs_used = 0;

        err = esp_littlefs_info(CONFIG_FILESYSTEM_PARTITION_LABEL,
                                &lfs_total, &lfs_used);
        total = lfs_total;
        used = lfs_used;
    }
    else
    {
        /* /sd: FATFS free-cluster query through the VFS mount, kept 64-bit
           all the way: the size_t cast that stood here wrapped a 32 GB card
           to 976 MB and the bench's 16 GB card to 2.83 GB (issue #982,
           2026-10-11) */
        uint64_t fat_total = 0;
        uint64_t fat_free = 0;

        err = esp_vfs_fat_info(FS_PREFIX_SD, &fat_total, &fat_free);
        total = fat_total;
        used = fat_total - fat_free;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    if (out_total != NULL)
    {
        *out_total = total;
    }

    if (out_used != NULL)
    {
        *out_used = used;
    }

    return ESP_OK;
}
