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
 * @file http_server_manager_private.h
 * @brief Internal declarations shared between http_server_manager .c files.
 *        Not part of the public API. Do not include from other components.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "http_server_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HSM_MAX_ASSET_TABLES 16
/* 48 overflowed 2026-07-06 when autopid's std_scan routes landed; 64
   overflowed 2026-07-07 when /api/usb/acm* + /api/scripts* landed — the
   rejects surface as 404s on whatever registers LATER (ws channels,
   /api/logs). Watch the "route buffer full" boot error when adding
   routes. Each slot is one httpd_uri_t (~16 B) in PSRAM — cheap. */
#define HSM_MAX_BUFFERED_URIS 96 /* 2026-07-08: 83 static routes + WS channels overflowed 80 (dtc+dbc routes; symptom = ws channel 404s) */
#define HSM_PATH_MAX 192

/* ---- match (http_server_manager_match.c, pure / host-testable) ------------ */

/**
 * @brief Normalize a request URI into @p out: strip query ('?') and fragment
 *        ('#'), reject traversal ("..") and over-long paths.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG on traversal/overflow.
 */
esp_err_t hsm_match_normalize(const char *uri, char *out, size_t out_len);

/**
 * @brief Find the first asset entry matching @p uri across the registered
 *        tables (earlier tables win; within a table, first entry wins).
 *
 * Exact entries match the whole URI. Prefix entries (uri ending in slash-then-asterisk) match
 * any path beneath them; @p fs_path_out then receives entry->fs_path joined
 * with the remainder. For exact FS entries @p fs_path_out is entry->fs_path.
 *
 * @param tables       Array of table pointers (each sentinel-terminated).
 * @param table_count  Number of tables.
 * @param fs_path_out  Resolved filesystem path (untouched for embedded entries).
 * @return Matching entry, or NULL.
 */
const http_asset_t *hsm_match_resolve(const http_asset_t *const *tables,
                                      size_t table_count, const char *uri,
                                      char *fs_path_out, size_t fs_path_len);

/** @brief MIME type from a path's extension ("application/octet-stream" fallback). */
const char *hsm_match_mime_from_path(const char *path);

/* ---- admin password (auth.c pure + settings.c, 2026-07-19) ----------------- */

/** PURE credential check (host-tested): Authorization header (Basic —
 *  any username, password part decides — or Bearer) OR the
 *  `wican_auth` cookie vs @p password. Empty/NULL password = open. */
bool hsm_auth_check(const char *auth_hdr, const char *cookie_hdr,
                    const char *password);

esp_err_t   hsm_settings_register(void);
bool        hsm_auth_enabled(void);   /* enabled AND a password stored  */
const char *hsm_auth_password(void);

/* ---- serve (http_server_manager_serve.c, target only) --------------------- */

/** @brief The GET catch-all handler installed last; walks the asset tables. */
esp_err_t hsm_serve_catchall(httpd_req_t *req);

/* Accessors serve.c uses to reach the registries owned by the core file. */
const http_asset_t *const *hsm_asset_tables(size_t *count_out);
http_asset_fetch_fn_t      hsm_asset_fetcher(void);

#ifdef __cplusplus
}
#endif
