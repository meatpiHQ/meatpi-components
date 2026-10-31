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
 * @file http_server_manager_serve.c
 * @brief The GET catch-all: resolves a request against the registered asset
 *        tables and serves embedded blobs or filesystem files, with
 *        fetch-on-miss caching and ETag/304 handling. Target-only.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "filesystem.h"

#include "http_server_manager_private.h"

static const char *TAG = "http_server_manager";

#define HSM_CHUNK_SIZE 4096
#define HSM_ETAG_LEN   48

/* Weak ETag from identity (size + mtime) for files on the filesystem. */
static void make_etag(char *buf, size_t buf_len, size_t size, long mtime)
{
    snprintf(buf, buf_len, "W/\"%lx-%lx\"", (unsigned long)size,
             (unsigned long)mtime);
}

/* The firmware build's identity for the embedded assets' ETag: the first
   four bytes of the app's ELF hash. An embedded file has no mtime, and its
   size alone let two builds collide: a device flashed with a new UI served
   304 to a browser holding the old one (2026-10-08, Ali: "flashed a new
   device but the changes did not take effect"). */
static const char *build_tag(void)
{
    static char tag[9];

    if (tag[0] == '\0')
    {
        const esp_app_desc_t *app = esp_app_get_description();

        snprintf(tag, sizeof(tag), "%02x%02x%02x%02x",
                 app->app_elf_sha256[0], app->app_elf_sha256[1],
                 app->app_elf_sha256[2], app->app_elf_sha256[3]);
    }

    return tag;
}

static void make_etag_embedded(char *buf, size_t buf_len, size_t size)
{
    snprintf(buf, buf_len, "W/\"%lx-%s\"", (unsigned long)size, build_tag());
}

/* True if the request carries If-None-Match equal to @p etag -> reply 304. */
static bool client_has_current(httpd_req_t *req, const char *etag)
{
    char inm[HSM_ETAG_LEN];

    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) != ESP_OK)
    {
        return false;
    }

    return strcmp(inm, etag) == 0;
}

static void set_common_headers(httpd_req_t *req, const http_asset_t *e,
                               const char *resolved_path, const char *etag,
                               bool embedded)
{
    const char *mime = (e->content_type != NULL)
                           ? e->content_type
                           : hsm_match_mime_from_path(resolved_path);

    httpd_resp_set_type(req, mime);

    if (e->content_encoding != NULL)
    {
        httpd_resp_set_hdr(req, "Content-Encoding", e->content_encoding);
    }

    /* embedded (the UI itself): cached, but asked about on every load, so
       a new firmware shows its page at once (a 304 costs one small
       request); files fetched once onto the filesystem keep the hour */
    httpd_resp_set_hdr(req, "Cache-Control",
                       embedded ? "no-cache" : "public, max-age=3600");
    httpd_resp_set_hdr(req, "ETag", etag);
}

static esp_err_t reply_304(httpd_req_t *req, const char *etag)
{
    httpd_resp_set_status(req, "304 Not Modified");
    httpd_resp_set_hdr(req, "ETag", etag);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t serve_embedded(httpd_req_t *req, const http_asset_t *e,
                                const char *uri)
{
    size_t size = (size_t)(e->data_end - e->data_start);
    char   etag[HSM_ETAG_LEN];

    make_etag_embedded(etag, sizeof(etag), size);

    if (client_has_current(req, etag))
    {
        return reply_304(req, etag);
    }

    set_common_headers(req, e, uri, etag, true);
    return httpd_resp_send(req, (const char *)e->data_start, size);
}

static esp_err_t serve_file(httpd_req_t *req, const http_asset_t *e,
                            const char *path)
{
    /* All file access goes through the filesystem component: it owns the
     * mounts, validates the path, and knows whether the backend (internal
     * or SD) is currently available (ARCHITECTURE §9.4). */
    FILE *f = filesystem_open(path, "rb");

    if (f == NULL)
    {
        /* Miss: fetch-once so the next request is served locally. */
        http_asset_fetch_fn_t fetch = hsm_asset_fetcher();

        if (e->source_url == NULL || fetch == NULL)
        {
            /* DEBUG: per-request path, scanners/bad links must not be able
               to flood the log pipeline at WARN (§10 hot-path rule) */
            ESP_LOGD(TAG, "404 %s (no local file%s)", path,
                     e->source_url != NULL ? ", no fetcher wired" : "");
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "miss %s -> fetching %s", path, e->source_url);
        esp_err_t err = fetch(e->source_url, path);

        if (err == ESP_OK)
        {
            f = filesystem_open(path, "rb");
        }

        if (f == NULL)
        {
            ESP_LOGE(TAG, "fetch failed for %s (%s)", e->source_url,
                     esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                "Not cached and fetch failed");
            return ESP_OK;
        }
    }

    /* the stream's reads, stat and close go through the filesystem's
       helpers: this task's stack is in PSRAM (2026-10-31) and a /data
       stream reads the flash with the cache off */
    struct stat st = { 0 };

    (void)filesystem_fstat(f, &st);

    char etag[HSM_ETAG_LEN];
    make_etag(etag, sizeof(etag), (size_t)st.st_size, (long)st.st_mtime);

    if (client_has_current(req, etag))
    {
        filesystem_fclose(f);
        return reply_304(req, etag);
    }

    /* internal: DMA / cache-off during FS read (SD DMA, LittleFS cache-off). */
    char *chunk = heap_caps_malloc(HSM_CHUNK_SIZE, MALLOC_CAP_INTERNAL);

    if (chunk == NULL)
    {
        filesystem_fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_OK;
    }

    set_common_headers(req, e, path, etag, false);

    esp_err_t result = ESP_OK;
    size_t    n;

    while ((n = filesystem_fread(chunk, HSM_CHUNK_SIZE, f)) > 0)
    {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK)
        {
            /* DEBUG: flaky clients can abort every request (§10 hot path) */
            ESP_LOGD(TAG, "client aborted %s", path);
            httpd_resp_sendstr_chunk(req, NULL);   /* terminate response */
            result = ESP_FAIL;
            break;
        }
    }

    if (result == ESP_OK)
    {
        httpd_resp_send_chunk(req, NULL, 0);   /* end of chunked body */
    }

    free(chunk);
    filesystem_fclose(f);
    return result;
}

esp_err_t hsm_serve_catchall(httpd_req_t *req)
{
    char uri[HSM_PATH_MAX];
    char fs_path[HSM_PATH_MAX];

    if (hsm_match_normalize(req->uri, uri, sizeof(uri)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_OK;
    }

    size_t table_count = 0;
    const http_asset_t *const *tables = hsm_asset_tables(&table_count);

    const http_asset_t *e =
        hsm_match_resolve(tables, table_count, uri, fs_path, sizeof(fs_path));

    if (e == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    if (e->data_start != NULL)
    {
        return serve_embedded(req, e, uri);
    }

    return serve_file(req, e, fs_path);
}
