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
 * @file autopid_dbc_private.h
 * @brief The DBC part of autopid_private.h: the pure DBC codec and the
 *        store / add-to-filters declarations. A FRAGMENT: included by
 *        autopid_private.h only; split out 2026-10-02 (700-line rule).
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

/* ---- pure: DBC codec (autopid_dbc_codec.c — host-tested; TASK_dbc.md) ----
 * Parse the BO_/SG_/SIG_VALTYPE_ subset; compile signals into
 * expression_parser expressions over filter payloads (B0 = first frame
 * data byte). Unsupported signals stay LISTED with a reason.           */

#define AP_DBC_MAX        4     /* stored .dbc files                    */
#define AP_DBC_NAME_LEN   33    /* message/signal identifiers           */
#define AP_DBC_FILE_MAX   (1024 * 1024)
#define AP_DBC_MSGS_MAX   400   /* per file                             */
#define AP_DBC_SIGS_MAX   3000  /* per file                             */

typedef struct
{
    uint32_t id;                /* 29-bit masked                        */
    bool     ext;
    bool     mux_complex;       /* extended multiplexing (SG_MUL_VAL_,
                                   m<N>M, or >1 switch) — unsupported   */
    uint8_t  dlc;
    char     name[AP_DBC_NAME_LEN];
} ap_dbc_msg_t;

typedef struct
{
    uint16_t msg;               /* index into the message table         */
    uint16_t start;             /* DBC start bit (raw numbering)        */
    uint8_t  len;
    bool     intel;             /* @1 = little endian                   */
    bool     is_signed;
    uint8_t  mux;               /* 0 plain, 1 m<N>, 2 M switch, 3 ext   */
    uint8_t  valtype;           /* 0 int, 1 float, 2 double             */
    uint16_t mux_val;           /* the N of m<N> (mux == 1)             */
    double   factor, offset, min, max;
    char     name[AP_DBC_NAME_LEN];
    char     unit[AP_UNIT_LEN];
} ap_dbc_sig_t;

/** Parse @p text. @return signal count (>=1) or -1 (err filled).
 *  Over-cap messages/signals are dropped silently (keep what fits). */
int ap_dbc_parse(const char *text, size_t len, ap_dbc_msg_t *msgs,
                 size_t msgs_cap, int *n_msgs, ap_dbc_sig_t *sigs,
                 size_t sigs_cap, char *err, size_t err_len);

/** Compile @p s into an expression (<= AP_EXPR_LEN incl. NUL).
 *  ESP_ERR_NOT_SUPPORTED sets @p reason (static string). */
esp_err_t ap_dbc_expr(const ap_dbc_sig_t *s, char *out, size_t out_cap,
                      const char **reason);

/** Multiplex precondition for @p s. Plain / M-switch signals: ESP_OK
 *  with expr_out = "" (no condition). m<N> signals: compiles the
 *  message's M switch as a RAW unsigned slice into @p expr_out and sets
 *  @p val_out = N — the runner evaluates the slice per frame and only
 *  applies the signal expression when it equals N. Extended
 *  multiplexing (mux 3 / msg.mux_complex) and switchless m<N> signals
 *  are ESP_ERR_NOT_SUPPORTED with @p reason set. */
esp_err_t ap_dbc_mux_cond(const ap_dbc_sig_t *s, const ap_dbc_msg_t *msgs,
                          const ap_dbc_sig_t *sigs, int n_sigs,
                          char *expr_out, size_t expr_cap,
                          float *val_out, const char **reason);

/** Reference decoder — host cross-check ONLY. */
double ap_dbc_decode_ref(const ap_dbc_sig_t *s, const uint8_t data[8]);

#ifndef AUTOPID_HOST_TEST
/* DBC store + cache + add-to-filters (autopid_dbc.c — TASK_dbc.md §4-5) */
void ap_dbc_init(void);                      /* autopid_init context     */
void ap_dbc_load_all(void);                  /* internal-stack ONLY (fs) */
esp_err_t ap_dbc_store(const char *name, const char *raw, size_t raw_len,
                       int *msgs_out, int *sigs_out, char *err,
                       size_t err_len);
esp_err_t ap_dbc_delete(const char *name);
cJSON *ap_dbc_list_json(void);
cJSON *ap_dbc_signals_json(const char *db, const char *q, int offset,
                           int limit);
esp_err_t ap_dbc_add(const char *db, const cJSON *signals,
                     const char *group, int monitor_ms, int period_ms,
                     cJSON **result, char *err, size_t err_len);
#endif
