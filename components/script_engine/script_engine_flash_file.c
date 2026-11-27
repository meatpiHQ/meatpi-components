/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_flash_file.c
 * @brief SD file access for the flash bindings. Every operation runs on
 *        an internal-RAM stack through flash_jobs_run(), not on the
 *        runner's PSRAM stack, as a precaution: one run that read an SD
 *        file through obd_file_read on the runner stack ended in a "Cache
 *        error" reset (2026-11-27, firmware 6.0.0-beta.12) whose cause is
 *        not established (SD is cache-safe by design; the backtrace was
 *        never symbolized). The bytes are bounced through an internal
 *        buffer, so the caller's buffer may be in PSRAM.
 */
#include "script_engine_flash.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "psa/crypto.h"

#include "external_storage.h"
#include "flash_jobs.h"

/* bytes per stdio call, on the job's stack: flash_jobs' worker has 4 KB
   and FATFS needs its share of it */
#define SE_FILE_BOUNCE 512

bool se_file_path_ok(const char *path)
{
    return path != NULL && strncmp(path, "/sd/", 4) == 0 &&
           strstr(path, "..") == NULL && strlen(path) < 128;
}

typedef struct
{
    const char    *path;
    size_t         off;
    uint8_t       *buf;     /* read target / write source                 */
    const uint8_t *src;
    size_t         len;
    size_t         done;
    bool           append;
    char          *hex;     /* sha256                                     */
} se_file_job_t;

static esp_err_t storage_ready(void)
{
    return external_storage_is_mounted() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t size_job(void *arg)
{
    se_file_job_t *j = arg;
    struct stat st;

    if (stat(j->path, &st) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    j->done = (size_t)st.st_size;
    return ESP_OK;
}

static esp_err_t read_job(void *arg)
{
    se_file_job_t *j = arg;
    uint8_t bounce[SE_FILE_BOUNCE]; /* internal: the job's stack */
    FILE *f = fopen(j->path, "rb");

    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = (fseek(f, (long)j->off, SEEK_SET) == 0) ? ESP_OK : ESP_FAIL;

    j->done = 0;

    while (err == ESP_OK && j->done < j->len)
    {
        size_t want = j->len - j->done;

        if (want > sizeof(bounce))
        {
            want = sizeof(bounce);
        }

        size_t got = fread(bounce, 1, want, f);

        memcpy(j->buf + j->done, bounce, got);
        j->done += got;

        if (got < want)
        {
            err = ferror(f) ? ESP_FAIL : ESP_OK;
            break; /* end of file */
        }
    }

    fclose(f);
    return err;
}

/* mkdir -p for the parents of a /sd path */
static void make_parents(const char *path)
{
    char dir[128];

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    for (char *p = dir + 4; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            (void)mkdir(dir, 0775);
            *p = '/';
        }
    }
}

static esp_err_t write_job(void *arg)
{
    se_file_job_t *j = arg;
    uint8_t bounce[SE_FILE_BOUNCE]; /* internal: the job's stack */

    make_parents(j->path);

    FILE *f = fopen(j->path, j->append ? "ab" : "wb");

    if (f == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;

    for (j->done = 0; j->done < j->len && err == ESP_OK;)
    {
        size_t n = j->len - j->done;

        if (n > sizeof(bounce))
        {
            n = sizeof(bounce);
        }

        memcpy(bounce, j->src + j->done, n);
        err = (fwrite(bounce, 1, n, f) == n) ? ESP_OK : ESP_FAIL;
        j->done += n;
    }

    if (fclose(f) != 0)
    {
        err = ESP_FAIL;
    }

    return err;
}

static esp_err_t sha_job(void *arg)
{
    se_file_job_t *j = arg;
    uint8_t bounce[SE_FILE_BOUNCE]; /* internal: the job's stack */
    FILE *f = fopen(j->path, "rb");

    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    psa_hash_operation_t op = psa_hash_operation_init();
    esp_err_t err = (psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS)
                        ? ESP_OK : ESP_FAIL;
    size_t got;

    while (err == ESP_OK && (got = fread(bounce, 1, sizeof(bounce), f)) > 0)
    {
        err = (psa_hash_update(&op, bounce, got) == PSA_SUCCESS) ? ESP_OK
                                                                 : ESP_FAIL;
    }

    if (err == ESP_OK && ferror(f))
    {
        err = ESP_FAIL;
    }

    fclose(f);

    uint8_t digest[32];
    size_t n = 0;

    if (err == ESP_OK &&
        psa_hash_finish(&op, digest, sizeof(digest), &n) == PSA_SUCCESS &&
        n == 32)
    {
        for (int i = 0; i < 32; i++)
        {
            snprintf(j->hex + 2 * i, 3, "%02x", digest[i]);
        }

        return ESP_OK;
    }

    psa_hash_abort(&op);
    return ESP_FAIL;
}

esp_err_t se_file_size(const char *path, size_t *size)
{
    se_file_job_t j = { .path = path };
    esp_err_t err = storage_ready();

    if (err == ESP_OK)
    {
        err = flash_jobs_run(size_job, &j);
    }

    *size = j.done;
    return err;
}

esp_err_t se_file_read(const char *path, size_t off, uint8_t *buf, size_t len,
                       size_t *got)
{
    se_file_job_t j = { .path = path, .off = off, .buf = buf, .len = len };
    esp_err_t err = storage_ready();

    if (err == ESP_OK)
    {
        err = flash_jobs_run(read_job, &j);
    }

    *got = j.done;
    return err;
}

esp_err_t se_file_write(const char *path, const uint8_t *d, size_t len,
                        bool append)
{
    se_file_job_t j = { .path = path, .src = d, .len = len, .append = append };
    esp_err_t err = storage_ready();

    return (err == ESP_OK) ? flash_jobs_run(write_job, &j) : err;
}

esp_err_t se_file_sha256(const char *path, char hex[65])
{
    se_file_job_t j = { .path = path, .hex = hex };
    esp_err_t err = storage_ready();

    hex[0] = '\0';
    (void)psa_crypto_init(); /* idempotent */
    return (err == ESP_OK) ? flash_jobs_run(sha_job, &j) : err;
}
