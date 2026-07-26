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
 * @file script_engine_obd.c
 * @brief The obd.* conversation surface behind the Berry bindings
 *        (SCRIPTING.md §4 item 2). PURE over an injected port so the
 *        claim/conversation state machine host-tests without the VM,
 *        uds_manager, or FreeRTOS (§4 item 5).
 *
 * Semantics:
 *  - se_obd_claim(addr): one claim at a time; delegates to the port's
 *    session_begin (uds_manager arms tester-present + holds the
 *    transport). Double-claim to the SAME addr is idempotent (extends);
 *    to a different addr = error (release first).
 *  - se_obd_request / isotp tx/rx REQUIRE the claim (scripts must be
 *    explicit about owning the bus conversation).
 *  - se_obd_autorelease(): called by the runner after EVERY script run
 *    (normal end, error, kill) — a script can never leak the claim.
 *    The hard time cap on a hold is the script runtime budget itself,
 *    which the runner already enforces.
 */
#include "script_engine_obd.h"

#include <string.h>

#include "esp_attr.h"

/* biggest single TransferData block we frame (2-byte 36+bsc header keeps
 * the ISO-TP PDU under the elm327_isotp 8192 payload ceiling) */
#define SE_OBD_XFER_MAX_BLOCK 4096

/* standard reflected CRC-32 (poly 0xEDB88320) — matches zlib.crc32 and
 * esp_rom_crc32_le, so the ECU's checkMemory compare agrees */
static uint32_t se_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;

    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];

        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }

    return ~crc;
}

static se_obd_port_t s_port;
static bool s_claimed;
static se_obd_addr_t s_addr;

void se_obd_set_port(const se_obd_port_t *port)
{
    if (port != NULL)
    {
        s_port = *port;
    }
    else
    {
        memset(&s_port, 0, sizeof(s_port));
    }

    s_claimed = false;
}

bool se_obd_claimed(void)
{
    return s_claimed;
}

int se_obd_claim(const se_obd_addr_t *addr)
{
    if (addr == NULL || s_port.session_begin == NULL)
    {
        return SE_OBD_ERR_ARG;
    }

    if (s_claimed)
    {
        /* same addr = extend (idempotent); different addr = misuse */
        if (memcmp(addr, &s_addr, sizeof(*addr)) == 0)
        {
            return SE_OBD_OK;
        }

        return SE_OBD_ERR_BUSY;
    }

    if (s_port.session_begin(addr) != 0)
    {
        return SE_OBD_ERR_IO;
    }

    s_addr = *addr;
    s_claimed = true;
    return SE_OBD_OK;
}

int se_obd_release(void)
{
    if (!s_claimed)
    {
        return SE_OBD_OK; /* idempotent */
    }

    s_claimed = false;

    if (s_port.session_end != NULL)
    {
        s_port.session_end();
    }

    return SE_OBD_OK;
}

void se_obd_autorelease(void)
{
    (void)se_obd_release();
}

int se_obd_request(const uint8_t *req, size_t req_len, uint32_t timeout_ms,
                   uint8_t *resp, size_t resp_cap, size_t *resp_len,
                   se_obd_outcome_t *out)
{
    if (req == NULL || req_len == 0 || resp == NULL || resp_len == NULL ||
        s_port.request == NULL)
    {
        return SE_OBD_ERR_ARG;
    }

    if (!s_claimed)
    {
        return SE_OBD_ERR_NOCLAIM;
    }

    return (s_port.request(&s_addr, req, req_len, timeout_ms,
                           resp, resp_cap, resp_len, out) == 0)
               ? SE_OBD_OK
               : SE_OBD_ERR_IO;
}

int se_obd_isotp_tx(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0 || s_port.isotp_tx == NULL)
    {
        return SE_OBD_ERR_ARG;
    }

    if (!s_claimed)
    {
        return SE_OBD_ERR_NOCLAIM;
    }

    return (s_port.isotp_tx(&s_addr, data, len, timeout_ms) == 0)
               ? SE_OBD_OK
               : SE_OBD_ERR_IO;
}

int se_obd_isotp_rx(uint8_t *out, size_t cap, size_t *out_len,
                    uint32_t timeout_ms)
{
    if (out == NULL || cap == 0 || out_len == NULL ||
        s_port.isotp_rx == NULL)
    {
        return SE_OBD_ERR_ARG;
    }

    if (!s_claimed)
    {
        return SE_OBD_ERR_NOCLAIM;
    }

    return (s_port.isotp_rx(&s_addr, out, cap, out_len, timeout_ms) == 0)
               ? SE_OBD_OK
               : SE_OBD_ERR_IO;
}

int se_obd_transfer_file(se_obd_read_fn read, void *ctx,
                         size_t size, size_t block_len, uint8_t first_bsc,
                         uint32_t timeout_ms, se_obd_xfer_result_t *out)
{
    static EXT_RAM_BSS_ATTR uint8_t req[2 + SE_OBD_XFER_MAX_BLOCK]; /* serialized; PSRAM */
    uint8_t resp[16];
    se_obd_xfer_result_t res = { .msg = NULL };

    if (out != NULL)
    {
        memset(out, 0, sizeof(*out));
    }

    if (read == NULL || s_port.request == NULL || size == 0 ||
        block_len == 0 || block_len > SE_OBD_XFER_MAX_BLOCK)
    {
        return SE_OBD_ERR_ARG;
    }

    if (!s_claimed)
    {
        return SE_OBD_ERR_NOCLAIM;
    }

    uint8_t bsc = first_bsc;
    size_t off = 0;

    while (off < size)
    {
        size_t want = (size - off < block_len) ? (size - off) : block_len;
        int got = read(ctx, off, req + 2, want);

        if (got < 0 || (size_t)got != want)
        {
            res.msg = "file read failed";
            if (out != NULL) { *out = res; }
            return SE_OBD_ERR_IO;
        }

        req[0] = 0x36;
        req[1] = bsc;

        size_t respn = 0;
        se_obd_outcome_t oc;

        int r = s_port.request(&s_addr, req, 2 + want, timeout_ms,
                               resp, sizeof(resp), &respn, &oc);

        /* accept: positive 0x76 with the echoed block-sequence counter */
        if (r != 0 || respn < 1 || resp[0] != 0x76 ||
            (respn >= 2 && resp[1] != bsc) || oc.negative)
        {
            res.msg = oc.negative ? "TransferData rejected (NRC)"
                                  : "TransferData no/bad response";
            res.blocks++;
            res.last_bsc = bsc;
            if (out != NULL) { *out = res; }
            return SE_OBD_ERR_IO;
        }

        res.crc32 = se_crc32(res.crc32, req + 2, want);
        res.sent += want;
        res.blocks++;
        res.last_bsc = bsc;
        bsc = (uint8_t)(bsc + 1);
        off += want;
    }

    if (out != NULL)
    {
        *out = res;
    }

    return SE_OBD_OK;
}
