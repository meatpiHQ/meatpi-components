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
 * @file ble_central_bench.h
 * @brief A NimBLE CENTRAL that connects to one WiCAN (`WiC_*`), tunes the
 *        link like esp-idf's throughput_app (MTU 517, DLE 251, 7.5 ms
 *        interval, PHY preference), pairs with the fixed passkey and runs
 *        timed GATT throughput tests: notify / write / read on the data
 *        pipe and Device Information, plus HTTP-over-BLE transfers on the
 *        `http` stream channel. The bench instrument for the BLE app
 *        platform; first host is the ECU simulator firmware.
 *
 *        Lifecycle (composition root): init -> register_http -> start.
 *        Everything else is driven over HTTP (`/api/ble_bench`), the CLI
 *        (`blebench`) or the API below. Results are polled, never blocked
 *        on: a test runs on the component's own task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    BCB_STATE_OFF = 0,      /* setting off or stack not up                 */
    BCB_STATE_IDLE,         /* stack up, no link                           */
    BCB_STATE_SCANNING,
    BCB_STATE_CONNECTING,   /* connect + tune + pair + discovery in flight */
    BCB_STATE_CONNECTED,    /* secured link, characteristics found         */
    BCB_STATE_RUNNING,      /* a test is in progress                       */
} bcb_state_t;

typedef enum
{
    BCB_MODE_NONE = 0,
    BCB_MODE_NOTIFY,        /* count FFF1 notifications (peer blasts)      */
    BCB_MODE_WRITE,         /* write-without-response to FFF2              */
    BCB_MODE_READ,          /* repeated reads of DIS 2A29                  */
    BCB_MODE_TUNNEL_UP,     /* HTTP-over-BLE upload of `size` bytes        */
    BCB_MODE_TUNNEL_DOWN,   /* HTTP-over-BLE download, byte-exact          */
} bcb_mode_t;

typedef struct
{
    char     name[32];      /* advertised name                             */
    char     addr[18];      /* peer address as seen (RPA)                  */
    uint16_t mtu;
    uint16_t itvl_units;    /* connection interval, 1.25 ms units          */
    uint8_t  phy_tx, phy_rx;/* 1 = 1M, 2 = 2M, 3 = coded, 0 = unknown      */
    uint16_t dle_tx, dle_rx;/* LL max octets agreed (0 = unknown)          */
    bool     secured;       /* encrypted + authenticated                   */
    bool     bonded;
    int8_t   rssi;
} bcb_peer_t;

typedef struct
{
    uint16_t fff1, fff2, fff3, fff4, dis_mfr;   /* value handles, 0 = absent */
    uint16_t fff1_cccd, fff3_cccd;              /* descriptor handles        */
} bcb_chars_t;

typedef struct
{
    bcb_mode_t mode;
    bool       ok;          /* completed without error                     */
    bool       running;
    uint32_t   bytes;       /* payload bytes moved                         */
    uint32_t   count;       /* PDUs / operations                           */
    uint32_t   ms;          /* wall time of the measured part              */
    uint32_t   kbps;        /* bytes * 8 / ms                              */
    uint32_t   errors;      /* stack refusals, retries that gave up        */
    uint32_t   retries;     /* ENOMEM / busy retries that succeeded        */
    int        http_status; /* tunnel modes: the response status          */
    bool       exact;       /* tunnel_down: payload matched               */
    uint32_t   holes;       /* tunnel: RSP_BODY counter gaps (lost PDUs)   */
    uint32_t   credits;     /* tunnel: CREDIT frames we sent (notify mode) */
    uint8_t    out;         /* tunnel: 1 = indications, 2 = notifications  */
    char       detail[64];  /* short human reason on failure               */
} bcb_result_t;

#define BCB_OUT_INDICATE 1  /* CCCD 0x0002 on FFF3                         */
#define BCB_OUT_NOTIFY   2  /* CCCD 0x0001 on FFF3 (ble_http v2 credits)   */

typedef struct
{
    uint32_t connects, disconnects, pair_ok, pair_fail;
    uint32_t notify_rx, notify_bytes, write_tx, write_bytes;
    uint32_t tunnel_frames_rx, tunnel_frames_tx, tunnel_resync;
    int      last_disconnect_reason;
} bcb_counters_t;

typedef struct
{
    bcb_state_t    state;
    bcb_peer_t     peer;
    bcb_chars_t    chars;
    bcb_result_t   last;
    bcb_counters_t counters;
} bcb_status_t;

/* ---- lifecycle ------------------------------------------------------------- */

esp_err_t ble_central_bench_init(void);           /* settings + log + CLI    */
esp_err_t ble_central_bench_register_http(void);  /* before the server runs  */
esp_err_t ble_central_bench_start(void);          /* brings the stack up iff enabled */
esp_err_t ble_central_bench_stop(void);

bool      ble_central_bench_is_enabled(void);

/* ---- control (any task; the work runs on the component's task) ------------- */

/** Scan for the configured target, connect, tune, pair, discover. */
esp_err_t ble_central_bench_connect(void);
esp_err_t ble_central_bench_disconnect(void);

/** Start a timed test on the connected link. `seconds` bounds every mode;
 *  `size` = bytes to move for the tunnel modes (and the blast request of
 *  the notify mode). ESP_ERR_INVALID_STATE without a secured link or
 *  while a test runs. */
/** out: BCB_OUT_* for the tunnel modes (how FFF3 is subscribed); other
 *  modes ignore it. 0 = notifications (the default since 2026-09-22). */
esp_err_t ble_central_bench_run(bcb_mode_t mode, uint32_t seconds, uint32_t size,
                                uint8_t out);

void      ble_central_bench_get_status(bcb_status_t *out);

const char *ble_central_bench_state_name(bcb_state_t s);
const char *ble_central_bench_mode_name(bcb_mode_t m);
bcb_mode_t  ble_central_bench_mode_parse(const char *s);   /* NONE if unknown */

#ifdef __cplusplus
}
#endif
