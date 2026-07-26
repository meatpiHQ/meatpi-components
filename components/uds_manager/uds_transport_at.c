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
 * @file uds_transport_at.c
 * @brief The AT-hex transaction shared by the obd_chip and elm327
 *        backends, plus the pure response parser (host-tested).
 *
 * Per target: set the protocol (ATSP6/7), tx header (ATSH / ATCP+ATSH
 * for 29-bit), rx filter (ATCRA), and headers-off + auto-formatting so
 * the chip does ISO-TP and hands us the pure UDS payload. The setup is
 * CACHED (see `cacheable`) for dedicated software engines so repeat
 * requests to the same ECU cost one AT command; the shared MIC always
 * re-sets it (autopid may change headers between our requests).
 */
#include "uds_transport.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "uds_proto.h"

/* ---- pure response parser --------------------------------------------------- */

static bool is_error_token(const char *tok, size_t len)
{
    /* ELM/MIC error phrases. Only match on a NON-hex char being present
     * (a pure hex+space line is data, never an error) so byte values
     * like FB/AC can't false-trigger. */
    bool has_alpha_word = false;

    for (size_t i = 0; i < len; i++)
    {
        char c = tok[i];

        if (c == '?')
        {
            return true; /* the ELM "did not understand" marker */
        }

        /* G-Z (letters that are NOT hex digits) => not a data line */
        char u = (char)toupper((unsigned char)c);

        if (u >= 'G' && u <= 'Z')
        {
            has_alpha_word = true;
        }
    }

    if (!has_alpha_word)
    {
        return false; /* pure hex/space/colon: it's data */
    }

    /* it has non-hex letters — confirm it's a known error phrase */
    static const char *ERR[] = { "NO DATA", "ERROR", "UNABLE", "BUFFER",
                                 "STOPPED", "SEARCHING", "BUS", "TIMEOUT",
                                 "RX", "TX", "CAN" };
    char up[32];
    size_t n = 0;

    for (size_t i = 0; i < len && n < sizeof(up) - 1; i++)
    {
        up[n++] = (char)toupper((unsigned char)tok[i]);
    }

    up[n] = '\0';

    for (size_t i = 0; i < sizeof(ERR) / sizeof(ERR[0]); i++)
    {
        if (strstr(up, ERR[i]) != NULL)
        {
            return true;
        }
    }

    /* has non-hex letters but no known phrase — treat as junk, reject */
    return true;
}

/* True if the line contains an ISO-TP frame index "<hex>:" (e.g. "0:"). */
static bool line_has_index(const char *p, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (p[i] == ':')
        {
            return true;
        }
    }

    return false;
}

/* Collect 2-hex-digit byte tokens from a line into out (after the ':'
 * if the line has a frame index). Returns false on overflow. */
static bool collect_line_bytes(const char *p, size_t len, uint8_t *out,
                               size_t cap, size_t *n)
{
    /* skip up to and including a frame-index ':' */
    for (size_t i = 0; i < len; i++)
    {
        if (p[i] == ':')
        {
            p += i + 1;
            len -= i + 1;
            break;
        }
    }

    char accum[3];
    size_t acc = 0;

    for (size_t i = 0; i <= len; i++)
    {
        char c = (i < len) ? p[i] : ' ';

        if (isxdigit((unsigned char)c))
        {
            accum[acc < 2 ? acc : 2] = c;
            acc = (acc < 2) ? acc + 1 : 3; /* >2 = not a byte pair */
            continue;
        }

        if (acc == 2)
        {
            if (*n >= cap)
            {
                return false;
            }

            accum[2] = '\0';
            unsigned v = 0;
            sscanf(accum, "%2x", &v);
            out[(*n)++] = (uint8_t)v;
        }

        acc = 0;
    }

    return true;
}

bool uds_at_parse_response(const char *resp, uint8_t *out, size_t cap,
                           size_t *out_len)
{
    if (resp == NULL || out == NULL || out_len == NULL)
    {
        return false;
    }

    *out_len = 0;

    /* First pass: is this the ISO-TP multiline format (any "N:" index)? */
    bool multiline = false;

    for (const char *p = resp; *p != '\0'; )
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n') eol++;

        if (line_has_index(p, (size_t)(eol - p)))
        {
            multiline = true;
            break;
        }

        p = (*eol == '\0') ? eol : eol + 1;
    }

    /* Second pass: collect bytes. In multiline mode the FIRST non-empty
     * line is the total-length prefix and is skipped. */
    size_t n = 0;
    bool skipped_len = !multiline;
    const char *p = resp;

    while (*p != '\0')
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n') eol++;

        size_t len = (size_t)(eol - p);

        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '>')) len--;

        while (len > 0 && *p == ' ') { p++; len--; }

        if (len > 0)
        {
            if (is_error_token(p, len))
            {
                return false;
            }

            if (!skipped_len)
            {
                skipped_len = true; /* drop the multiline length prefix */
            }
            else if (!collect_line_bytes(p, len, out, cap, &n))
            {
                return false;
            }
        }

        p = (*eol == '\0') ? eol : eol + 1;
    }

    *out_len = n;
    return n > 0;
}

/* ---- shared AT transaction -------------------------------------------------- */

static void hex_no_space(const uint8_t *b, size_t len, char *out, size_t cap)
{
    static const char H[] = "0123456789ABCDEF";
    size_t o = 0;

    for (size_t i = 0; i < len && o + 2 < cap; i++)
    {
        out[o++] = H[b[i] >> 4];
        out[o++] = H[b[i] & 0x0F];
    }

    out[o] = '\0';
}

esp_err_t uds_at_transceive(uds_at_request_fn req_fn, bool cacheable,
                            const uds_addr_t *addr,
                            const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len,
                            uint32_t p2_ms, uint32_t p2star_ms,
                            uint8_t *pending_out)
{
    if (req_fn == NULL || addr == NULL || req == NULL || req_len == 0 ||
        resp == NULL || resp_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (pending_out != NULL)
    {
        *pending_out = 0; /* the MIC/ELM consumes 0x78 internally */
    }

    /* give the chip the extended window: it waits out responsePending
     * itself, so use p2* (fall back to p2) as the request timeout */
    uint32_t timeout_ms = (p2star_ms > p2_ms) ? p2star_ms : p2_ms;

    char at[48];
    char rbuf[512];

    /* Setup caching: sending protocol/header/filter/mode on EVERY request
     * is ~7 AT round-trips — fine for the fast MIC, too slow through a
     * software AT engine. Re-send the setup only when the target
     * (req_fn+addr) changes; steady-state = one AT command (the hex). */
    static uds_at_request_fn s_last_fn;
    static uint32_t s_last_tx, s_last_rx;
    static bool s_last_ext, s_configured;

    bool need_setup = !cacheable || !s_configured || s_last_fn != req_fn ||
                      s_last_tx != addr->tx_id || s_last_rx != addr->rx_id ||
                      s_last_ext != addr->ext_id;

    if (need_setup)
    {
        /* CAN protocol: ISO 15765-4, 11-bit (SP6) or 29-bit (SP7), 500k */
        (void)req_fn(addr->ext_id ? "ATSP7" : "ATSP6", rbuf, sizeof(rbuf),
                     800);

        if (addr->ext_id)
        {
            snprintf(at, sizeof(at), "ATCP%02lX",
                     (unsigned long)((addr->tx_id >> 24) & 0x1F));
            (void)req_fn(at, rbuf, sizeof(rbuf), 800);
            snprintf(at, sizeof(at), "ATSH%06lX",
                     (unsigned long)(addr->tx_id & 0xFFFFFF));
            (void)req_fn(at, rbuf, sizeof(rbuf), 800);
            snprintf(at, sizeof(at), "ATCRA%08lX",
                     (unsigned long)addr->rx_id);
        }
        else
        {
            snprintf(at, sizeof(at), "ATSH%03lX",
                     (unsigned long)(addr->tx_id & 0x7FF));
            (void)req_fn(at, rbuf, sizeof(rbuf), 800);
            snprintf(at, sizeof(at), "ATCRA%03lX",
                     (unsigned long)(addr->rx_id & 0x7FF));
        }

        (void)req_fn(at, rbuf, sizeof(rbuf), 800);

        /* headers off, ISO-TP auto-formatting on, spaces on (parseable) */
        (void)req_fn("ATH0", rbuf, sizeof(rbuf), 800);
        (void)req_fn("ATCAF1", rbuf, sizeof(rbuf), 800);
        (void)req_fn("ATS1", rbuf, sizeof(rbuf), 800);

        s_last_fn = req_fn;
        s_last_tx = addr->tx_id;
        s_last_rx = addr->rx_id;
        s_last_ext = addr->ext_id;
        s_configured = true;
    }

    /* the request */
    char cmd[2 * 64 + 1];

    hex_no_space(req, req_len, cmd, sizeof(cmd));

    esp_err_t err = req_fn(cmd, rbuf, sizeof(rbuf), timeout_ms);

    if (err != ESP_OK)
    {
        s_configured = false; /* re-setup after any failure */
        return err;
    }

    if (!uds_at_parse_response(rbuf, resp, resp_cap, resp_len))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
