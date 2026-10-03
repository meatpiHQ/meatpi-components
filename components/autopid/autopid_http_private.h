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
 * @file autopid_http_private.h
 * @brief Internal contract between the autopid HTTP files: the route
 *        table lives in autopid_http.c, the DTC, DBC and vehicle store
 *        handlers in autopid_http_dtc.c / autopid_http_dbc.c /
 *        autopid_http_vehicles.c (split 2026-10-01 to keep every file
 *        under the standard's 700 lines), and the response helpers are
 *        shared. Not part of the component's public API.
 */
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"



/* ---- shared response helpers (autopid_http.c) ------------------------------- */

/** Serialise @p obj (consumed) as the JSON body. */
esp_err_t ap_http_send_json(httpd_req_t *req, cJSON *obj);
/** `{"error": msg}` with the given status line. */
esp_err_t ap_http_send_error(httpd_req_t *req, const char *status,
                             const char *msg);
/** 409 with the bus guard's own sentence: a chip job it refused. */
esp_err_t ap_http_send_guard_error(httpd_req_t *req);
/** The file at @p path verbatim, or @p dflt when it is missing or empty. */
esp_err_t ap_http_send_file(httpd_req_t *req, const char *path,
                            const char *dflt);
/** One query-string value (autopid_http_dtc.c). */
bool ap_http_query_param(httpd_req_t *req, const char *key, char *out,
                        size_t out_cap);

/* ---- test-a-PID (autopid_http_test.c, HTTP_API.md 6e4): one shot through
 * the runner, or the J1939 store for a PGN: command ----------------------- */
esp_err_t test_post_handler(httpd_req_t *req);

/* ---- DTC routes (autopid_http_dtc.c, HTTP_API.md 6e4b) ---------------------- */
esp_err_t dtc_clear_post_handler(httpd_req_t *req);
esp_err_t dtc_db_delete_handler(httpd_req_t *req);
esp_err_t dtc_db_get_handler(httpd_req_t *req);
esp_err_t dtc_db_post_handler(httpd_req_t *req);
esp_err_t dtc_db_search_handler(httpd_req_t *req);
esp_err_t dtc_get_handler(httpd_req_t *req);
esp_err_t dtc_lookup_handler(httpd_req_t *req);
esp_err_t dtc_scan_post_handler(httpd_req_t *req);

/* ---- DBC routes (autopid_http_dbc.c, HTTP_API.md 6e4c) ---------------------- */
esp_err_t dbc_add_handler(httpd_req_t *req);
esp_err_t dbc_delete_handler(httpd_req_t *req);
esp_err_t dbc_get_handler(httpd_req_t *req);
esp_err_t dbc_post_handler(httpd_req_t *req);
esp_err_t dbc_signals_handler(httpd_req_t *req);

/* ---- vehicle store routes (autopid_http_vehicles.c, HTTP_API.md 6e4):
 * one wildcard URI per method, the path dispatched inside -------------- */
esp_err_t vehicles_get_handler(httpd_req_t *req);     /* GET  .../vehicles */
esp_err_t vehicles_post_handler(httpd_req_t *req);    /* detect, activate  */
esp_err_t vehicles_put_handler(httpd_req_t *req);     /* PUT  .../<key>    */
esp_err_t vehicles_delete_handler(httpd_req_t *req);  /* DELETE .../<key>  */
