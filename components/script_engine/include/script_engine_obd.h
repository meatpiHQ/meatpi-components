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
 * @file script_engine_obd.h
 * @brief obd.* conversation core (pure over an injected port — see
 *        script_engine_obd.c). The Berry glue lives in
 *        script_engine_bind.c; the device port install in
 *        script_engine.c; host tests inject fakes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    SE_OBD_OK          = 0,
    SE_OBD_ERR_ARG     = -1,
    SE_OBD_ERR_BUSY    = -2, /* claimed to a DIFFERENT addr            */
    SE_OBD_ERR_NOCLAIM = -3, /* request/isotp without an active claim  */
    SE_OBD_ERR_IO      = -4, /* port-level failure (timeout, transport)*/
};

/* Mirror of uds_addr_t without the uds_manager dependency (host tests). */
typedef struct
{
    uint32_t tx_id;
    uint32_t rx_id;
    bool     ext_id;
} se_obd_addr_t;

/* Outcome mirror of uds_result_t's script-relevant fields. */
typedef struct
{
    bool        negative;
    uint8_t     nrc;
    const char *nrc_name;
    uint8_t     pending_count;
} se_obd_outcome_t;

/* The injected port: device glue -> uds_manager; host tests -> fakes.
 * All fns return 0 on success. */
typedef struct
{
    int (*session_begin)(const se_obd_addr_t *addr);
    void (*session_end)(void);
    int (*request)(const se_obd_addr_t *addr,
                   const uint8_t *req, size_t req_len, uint32_t timeout_ms,
                   uint8_t *resp, size_t resp_cap, size_t *resp_len,
                   se_obd_outcome_t *out);
    int (*isotp_tx)(const se_obd_addr_t *addr,
                    const uint8_t *data, size_t len, uint32_t timeout_ms);
    int (*isotp_rx)(const se_obd_addr_t *addr,
                    uint8_t *out, size_t cap, size_t *out_len,
                    uint32_t timeout_ms);
} se_obd_port_t;

void se_obd_set_port(const se_obd_port_t *port);

bool se_obd_claimed(void);
int  se_obd_claim(const se_obd_addr_t *addr);
int  se_obd_release(void);
/** Runner hook: guaranteed release after every script run. */
void se_obd_autorelease(void);

int se_obd_request(const uint8_t *req, size_t req_len, uint32_t timeout_ms,
                   uint8_t *resp, size_t resp_cap, size_t *resp_len,
                   se_obd_outcome_t *out);
int se_obd_isotp_tx(const uint8_t *data, size_t len, uint32_t timeout_ms);
int se_obd_isotp_rx(uint8_t *out, size_t cap, size_t *out_len,
                    uint32_t timeout_ms);

/* ---- reflash TransferData streamer (SD file → 0x36 loop) ------------------ */

/* A blocking chunk read from an already-opened firmware source. Returns
 * bytes read (< len only at EOF), or < 0 on error. Injected so the core
 * host-tests over a byte array with no filesystem. */
typedef int (*se_obd_read_fn)(void *ctx, size_t offset, uint8_t *out,
                              size_t len);

typedef struct
{
    const char *msg;      /* NULL on success                           */
    size_t      sent;     /* payload bytes transferred                 */
    uint32_t    blocks;   /* number of 0x36 requests                   */
    uint32_t    crc32;    /* CRC-32 over the transferred payload        */
    uint8_t     last_bsc; /* block sequence counter of the last block  */
} se_obd_xfer_result_t;

/* Stream @p size bytes from @p read(ctx,...) as UDS TransferData:
 *   36 <bsc> <block>   (block_len payload, last block short), await 76 <bsc>.
 * Requires an active claim AND allow_reflash (checked by the caller). The
 * caller frames RequestDownload/TransferExit/checkMemory around this. */
int se_obd_transfer_file(se_obd_read_fn read, void *ctx,
                         size_t size, size_t block_len, uint8_t first_bsc,
                         uint32_t timeout_ms, se_obd_xfer_result_t *out);

#ifdef __cplusplus
}
#endif
