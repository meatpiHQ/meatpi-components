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
 * @file cert_manager.h
 * @brief WiCAN TLS certificate-set owner (service component).
 *
 * Owns named certificate SETS (rewrite of the legacy cert_manager) on
 * the filesystem: `/data/certs/<set>/` holding up to three PEM parts —
 * `ca.pem` (broker/server verification), `client.crt` + `client.key`
 * (mutual TLS). Consumers (mqtt_manager's `cert_set` setting today; VPN
 * or HTTPS clients later) borrow NUL-terminated, PSRAM-cached contents
 * by set name; they never touch the files.
 *
 * Upload/list/delete via `/api/certs` (own routes, HTTP_API.md §6g).
 * Parts are validated as PEM at upload. **There is deliberately no HTTP
 * read-back — key material never leaves the device.** (v1 trust note:
 * the generic `/api/fs` browser can still reach `/data/certs` — same
 * AP-trust model as the rest of the API until the auth story; revisit
 * together.)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CERT_MANAGER_MAX_SETS     10
#define CERT_MANAGER_NAME_MAX     24   /* set name chars (a-z0-9_-)      */
#define CERT_MANAGER_PEM_MAX      8192 /* per part                       */

typedef enum
{
    CERT_MANAGER_CA = 0,     /* ca.pem                                   */
    CERT_MANAGER_CLIENT_CERT,/* client.crt                               */
    CERT_MANAGER_CLIENT_KEY, /* client.key                               */
} cert_manager_part_t;

typedef struct
{
    char name[CERT_MANAGER_NAME_MAX + 1];
    bool has_ca;
    bool has_client_cert;
    bool has_client_key;
} cert_manager_set_info_t;

/** Register the log descriptor. No filesystem access. */
esp_err_t cert_manager_init(void);

/** Ensure the certs directory exists and scan the sets. */
esp_err_t cert_manager_start(void);
esp_err_t cert_manager_stop(void);

/**
 * Borrow a part's content: NUL-terminated PEM in PSRAM, loaded on first
 * use and cached until the set changes. @p len_out (nullable) receives
 * strlen+1 — the length TLS configs want for PEM. The pointer stays
 * valid until the set is deleted/re-uploaded; consumers that connect at
 * start() and never re-read (mqtt) are safe by construction.
 * ESP_ERR_NOT_FOUND when the set or part doesn't exist.
 */
esp_err_t cert_manager_get(const char *set, cert_manager_part_t part,
                           const char **out, size_t *len_out);

/** Fill @p out (capacity CERT_MANAGER_MAX_SETS); count via @p n_out. */
esp_err_t cert_manager_list(cert_manager_set_info_t *out, size_t *n_out);

/** True when @p set exists with at least a CA part. */
bool cert_manager_set_usable(const char *set);

/** Register the /api/certs routes (composition root, HTTP builds). */
esp_err_t cert_manager_register_http(void);

#ifdef __cplusplus
}
#endif
