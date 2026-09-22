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
 * @file ble_central_bench_private.h
 * @brief Seams between the files of ble_central_bench: settings (config),
 *        the NimBLE central (gap), the core (state, the test task) and the
 *        CLI/HTTP surfaces. Nothing here is public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "ble_central_bench.h"

#define BCB_TAG            "ble_central_bench"
#define BCB_TASK_STACK     8192        /* PSRAM stack: no flash writes here */
#define BCB_RX_STORAGE     32768       /* FFF3 (http OUT) reassembly buffer  */
#define BCB_PHY_1M         0x01u
#define BCB_PHY_2M         0x02u
#define BCB_SUPERVISION    600         /* 6 s, 10 ms units (the reference)  */
#define BCB_OP_TIMEOUT_MS  5000        /* blocking GATT op (read/write rsp)  */
#define BCB_DLE_OCTETS     251
#define BCB_DLE_TIME_US    2120
/* the WiCAN caps every attribute payload at min(490, MTU-3) (BLE_API.md 4.3);
   a 514-byte write-without-response exceeds NimBLE's 512-byte attribute
   limit and is dropped silently (bench, 2026-09-21) */
#define BCB_PAYLOAD_CAP    490
static inline size_t bcb_payload_cap(uint16_t mtu)
{
    size_t c = mtu > 3 ? (size_t)mtu - 3 : 20;

    return c > BCB_PAYLOAD_CAP ? BCB_PAYLOAD_CAP : c;
}

/* ---- settings ------------------------------------------------------------------ */

typedef struct
{
    bool     enabled;
    char     target[32];       /* advertised-name prefix or full name        */
    uint32_t passkey;
    uint16_t mtu;
    uint16_t conn_itvl_units;  /* 1.25 ms units                              */
    uint16_t ce_len_units;     /* 0.625 ms units (max; min = half)           */
    bool     dle;
    uint8_t  phy_mask;         /* BCB_PHY_*                                  */
    bool     cli;
} bcb_config_t;

const bcb_config_t *bcb_settings_config(void);
bool      bcb_settings_is_configured(void);
esp_err_t bcb_settings_register(void);
esp_err_t ble_central_bench_register_cli(void);

/* ---- core (state + the test task) ----------------------------------------------- */

void          bcb_core_lock(void);
void          bcb_core_unlock(void);
bcb_status_t *bcb_core_status(void);          /* hold the lock while touching */
void          bcb_core_set_state(bcb_state_t s);

/* a small ring of diagnostic lines (also logged): the simulator has no
   console on the bench, so GET /api/ble_bench carries the last steps */
#define BCB_DIAG_LINES 24
#define BCB_DIAG_LEN   96
void          bcb_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int           bcb_diag_lines(char (*out)[BCB_DIAG_LEN], int cap);  /* oldest first */

/* the FFF3 (http OUT) byte stream for the tunnel client, filled by the host task */
StreamBufferHandle_t bcb_core_http_rx(void);

/* ---- modes (the timed tests, bench task) --------------------------------------- */

void bcb_mode_notify(uint32_t seconds, uint32_t size, bcb_result_t *r);
void bcb_mode_write(uint32_t seconds, bcb_result_t *r);
void bcb_mode_read(uint32_t seconds, bcb_result_t *r);
void bcb_mode_tunnel(bcb_mode_t mode, uint32_t seconds, uint32_t size, uint8_t out,
                     bcb_result_t *r);
void bcb_modes_on_fff1(size_t n);            /* host task: FFF1 notification bytes */

/* Callbacks from the NimBLE host task (flag/count/copy only): */
void bcb_core_on_connected(const char *name, const char *addr, int8_t rssi);
void bcb_core_on_secured(bool ok, bool bonded);
void bcb_core_on_discovered(const bcb_chars_t *chars, bool ok);
void bcb_core_on_disconnected(int reason);
void bcb_core_on_notify(uint16_t attr_handle, const uint8_t *data, size_t n);
void bcb_core_on_link_params(uint16_t mtu, uint16_t itvl_units,
                             uint8_t phy_tx, uint8_t phy_rx,
                             uint16_t dle_tx, uint16_t dle_rx);

/* ---- discovery (host task chain, ends in bcb_core_on_discovered) ---------------- */

void bcb_disc_start(uint16_t conn_handle);

/* ---- gap (the NimBLE central) ------------------------------------------------------ */

esp_err_t bcb_gap_start(void);               /* controller + host up          */
void      bcb_gap_stop(void);
esp_err_t bcb_gap_connect(void);             /* scan + connect (async)        */
esp_err_t bcb_gap_disconnect(void);
bool      bcb_gap_connected(void);

/** Write-without-response; retries ENOMEM with a 1 ms yield up to
 *  @p max_wait_ms. Returns the retries taken (>= 0) or -1 on failure. */
int       bcb_gap_write_nr(uint16_t handle, const uint8_t *data, size_t n,
                           uint32_t max_wait_ms);
/** Write with response (blocking, BCB_OP_TIMEOUT_MS). */
esp_err_t bcb_gap_write(uint16_t handle, const uint8_t *data, size_t n);
/** Read (blocking): bytes copied, or <0. */
int       bcb_gap_read(uint16_t handle, uint8_t *dst, size_t cap);
/** CCCD: 1 = notify, 2 = indicate, 0 = off. */
esp_err_t bcb_gap_subscribe(uint16_t cccd_handle, uint16_t value);
uint16_t  bcb_gap_mtu(void);                 /* negotiated ATT MTU            */
