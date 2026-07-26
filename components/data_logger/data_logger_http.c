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
 * @file data_logger_http.c
 * @brief The optional /api/logger status route (§9.1). File
 *        browse/download/delete deliberately NOT duplicated here — the
 *        files live under /sd/logs and the existing /api/fs file
 *        manager routes cover them.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "data_logger.h"
#include "data_logger_private.h"

static const char *TAG = "data_logger";

static esp_err_t logger_handler(httpd_req_t *req)
{
    data_logger_stats_t st;
    char body[640];

    (void)data_logger_stats(&st);
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"running\":%s,\"paused\":%s,"
             "\"storage_ok\":%s,"
             "\"file\":\"%s\",\"file_rows\":%lu,\"files\":%lu,"
             "\"queued\":%lu,"
             "\"written\":%lu,\"dropped\":%lu,\"errors\":%lu,"
             "\"rotations\":%lu,"
             "\"can\":{\"enabled\":%s,\"file\":\"%s\","
             "\"file_rows\":%lu,\"files\":%lu,\"queued\":%lu,"
             "\"frames_written\":%lu,\"frames_dropped\":%lu,"
             "\"rotations\":%lu},"
             "\"dir\":\"/sd/logs\"}",
             st.enabled ? "true" : "false",
             st.running ? "true" : "false",
             st.paused ? "true" : "false",
             st.storage_ok ? "true" : "false",
             st.file,
             (unsigned long)st.file_rows, (unsigned long)st.files,
             (unsigned long)st.queued,
             (unsigned long)st.written, (unsigned long)st.dropped,
             (unsigned long)st.errors, (unsigned long)st.rotations,
             st.can_enabled ? "true" : "false",
             st.can_file,
             (unsigned long)st.can_file_rows,
             (unsigned long)st.can_files,
             (unsigned long)st.can_queued,
             (unsigned long)st.frames_written,
             (unsigned long)st.frames_dropped,
             (unsigned long)st.can_rotations);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/logger/gate {"enabled":bool} — runtime (non-persisted)
 * logging gate, the HTTP twin of the logger.enable/logger.disable event
 * actions (API-first §1b: anything a rule can do, a client can do). The
 * observable state is GET /api/logger's "paused" field. */
static esp_err_t gate_post_handler(httpd_req_t *req)
{
    char body[128];
    size_t len = req->content_len;

    if (len == 0 || len >= sizeof(body))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "missing/oversized body");
        return ESP_FAIL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "body read failed");
            return ESP_FAIL;
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    /* one bool field — a full cJSON parse is overkill */
    bool on;

    if (strstr(body, "\"enabled\"") == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need bool 'enabled'");
        return ESP_FAIL;
    }
    else if (strstr(body, "true") != NULL)
    {
        on = true;
    }
    else if (strstr(body, "false") != NULL)
    {
        on = false;
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need bool 'enabled'");
        return ESP_FAIL;
    }

    dl_runtime_gate(on);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ---- GET /api/logger/export?stream=params&since=<cursor>&limit=<n> --------
 * Device-contract ask #8: incremental NDJSON export of the params stream
 * (jsonl engine only). Cursor = "<file-epoch>:<byte-offset>", opaque to
 * the client; the FINAL line of every response is a meta record
 * {"_cursor":"<next>","more":true|false} — resend it as `since` to
 * continue. Files are epoch-named (dl_<epoch>.jsonl) so the cursor
 * survives rotation: a retired epoch resumes at the next newer file.
 * Reads are budgeted (lines + bytes) and cut on record boundaries; the
 * ACTIVE file's trailing partial line is never emitted. /sd reads are
 * httpd-task-safe (only internal flash has the cache corollary). */
#define DL_EXPORT_MAX_FILES  64
#define DL_EXPORT_BYTE_CAP   16384
#define DL_EXPORT_LINE_CAP   2000
#define DL_EXPORT_LINE_DEF   500

static int export_list_epochs(int64_t *out, int cap)
{
    DIR *d = opendir(DL_DIR);

    if (d == NULL)
    {
        return 0;
    }

    int n = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL && n < cap)
    {
        int64_t epoch;
        const char *dot = strrchr(e->d_name, '.');

        if (dot != NULL && strcmp(dot, ".jsonl") == 0 &&
            strncmp(e->d_name, DL_PREFIX_PARAM,
                    strlen(DL_PREFIX_PARAM)) == 0 &&
            dl_files_parse(e->d_name, &epoch))
        {
            out[n++] = epoch;
        }
    }

    closedir(d);

    /* ascending (oldest first) — insertion sort, n is small */
    for (int i = 1; i < n; i++)
    {
        int64_t v = out[i];
        int j = i - 1;

        while (j >= 0 && out[j] > v)
        {
            out[j + 1] = out[j];
            j--;
        }

        out[j + 1] = v;
    }

    return n;
}

static esp_err_t export_get_handler(httpd_req_t *req)
{
    const dl_cfg_t *cfg = dl_settings_config();

    if (cfg == NULL || strcmp(cfg->format, "jsonl") != 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "params stream is not jsonl");
        return ESP_FAIL;
    }

    char query[128] = "";
    char val[48];
    int64_t since_epoch = 0;
    long since_off = 0;
    int limit = DL_EXPORT_LINE_DEF;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "stream", val, sizeof(val)) ==
                ESP_OK && strcmp(val, "params") != 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "only stream=params");
            return ESP_FAIL;
        }

        if (httpd_query_key_value(query, "since", val, sizeof(val)) ==
                ESP_OK)
        {
            sscanf(val, "%lld:%ld", (long long *)&since_epoch, &since_off);
        }

        if (httpd_query_key_value(query, "limit", val, sizeof(val)) ==
                ESP_OK)
        {
            limit = atoi(val);

            if (limit < 1 || limit > DL_EXPORT_LINE_CAP)
            {
                limit = DL_EXPORT_LINE_DEF;
            }
        }
    }

    int64_t epochs[DL_EXPORT_MAX_FILES];
    int nfiles = export_list_epochs(epochs, DL_EXPORT_MAX_FILES);

    /* first file at/after the cursor (a retired epoch resumes at the
       next newer file, offset 0) */
    int idx = 0;

    while (idx < nfiles && epochs[idx] < since_epoch)
    {
        idx++;
    }

    if (idx < nfiles && epochs[idx] != since_epoch)
    {
        since_off = 0;
    }

    httpd_resp_set_type(req, "application/x-ndjson");

    long off = since_off;
    int lines_left = limit;
    int bytes_left = DL_EXPORT_BYTE_CAP;
    bool more = false;
    bool gated_here = false;
    int64_t cur_epoch = (idx < nfiles) ? epochs[idx] : since_epoch;

    while (idx < nfiles && lines_left > 0 && bytes_left > 0)
    {
        char fname[40];
        char path[64];

        cur_epoch = epochs[idx];
        dl_files_make(fname, sizeof(fname), DL_PREFIX_PARAM, cur_epoch,
                      ".jsonl");
        snprintf(path, sizeof(path), DL_DIR "/%s", fname);

        FILE *f = fopen(path, "rb");

        if (f == NULL && !gated_here)
        {
            /* the ACTIVE file is exclusively held by the writer (FATFS
               FS_LOCK) — pause the gate; the writer closes its files
               within one ~200 ms loop. Restored after the read unless a
               logger.disable rule had already paused it. */
            data_logger_stats_t st;

            if (data_logger_stats(&st) == ESP_OK && !st.paused)
            {
                dl_runtime_gate(false);
                gated_here = true;

                for (int t = 0; t < 10 && f == NULL; t++)
                {
                    vTaskDelay(pdMS_TO_TICKS(60));
                    f = fopen(path, "rb");
                }
            }
        }

        if (f == NULL)
        {
            idx++;
            off = 0;
            continue;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);

        fseek(f, off, SEEK_SET);

        while (lines_left > 0 && bytes_left > 0 && off < fsize)
        {
            char buf[1024];
            size_t want = sizeof(buf);

            if ((long)want > fsize - off)
            {
                want = (size_t)(fsize - off);
            }

            if ((int)want > bytes_left)
            {
                want = (size_t)bytes_left;
            }

            size_t n = fread(buf, 1, want, f);

            if (n == 0)
            {
                break;
            }

            /* cut on the last record boundary, and no more lines than
               the budget allows */
            size_t keep = 0;
            int newlines = 0;

            for (size_t i = 0; i < n; i++)
            {
                if (buf[i] == '\n')
                {
                    newlines++;
                    keep = i + 1;

                    if (newlines == lines_left)
                    {
                        break;
                    }
                }
            }

            if (keep == 0)
            {
                /* no boundary in this window: a >1 KB record mid-file is
                   forwarded raw (offsets stay exact); a trailing partial
                   line (active file mid-append) is left for next time */
                if (off + (long)n < fsize && n == sizeof(buf))
                {
                    keep = n;
                }
                else
                {
                    break;
                }
            }

            httpd_resp_send_chunk(req, buf, keep);
            off += (long)keep;
            bytes_left -= (int)keep;
            lines_left -= newlines;
        }

        bool file_done = (off >= fsize);

        fclose(f);

        if (!file_done)
        {
            more = true; /* budget hit or partial tail pending */
            break;
        }

        if (idx + 1 < nfiles)
        {
            idx++;
            off = 0;
        }
        else
        {
            break; /* fully caught up on the newest file */
        }
    }

    if (gated_here)
    {
        dl_runtime_gate(true);
    }

    char meta[96];

    snprintf(meta, sizeof(meta),
             "{\"_cursor\":\"%lld:%ld\",\"more\":%s}\n",
             (long long)cur_epoch, off, more ? "true" : "false");
    httpd_resp_send_chunk(req, meta, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t data_logger_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/logger", .method = HTTP_GET,
          .handler = logger_handler },
        { .uri = "/api/logger/gate", .method = HTTP_POST,
          .handler = gate_post_handler },
        { .uri = "/api/logger/export", .method = HTTP_GET,
          .handler = export_get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/logger registered");
    }

    return err;
}
