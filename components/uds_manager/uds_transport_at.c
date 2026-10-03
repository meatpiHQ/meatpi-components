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
 * @brief The AT-hex transaction behind the obd_chip transport, plus the
 *        pure response parser (host-tested).
 *
 * Per target: set the protocol (ATTP6/7 at 500k, ATTP8/9 at 250k,
 * RAM-only), tx header (ATSH /
 * ATCP+ATSH for 29-bit), rx filter (ATCRA), headers-off + auto-formatting
 * so the chip does ISO-TP and hands us the pure UDS payload, and the
 * chip's own reply wait (ATST) from p2. The setup goes out when the
 * caller says so (uds_at_transceive_ex skip_setup=false): the MIC is
 * shared with autopid, which re-addresses it between our transactions;
 * the obd_chip transport detects that with the driver's tx_bytes counter
 * and skips the setup in a steady conversation (one request = one AT
 * line). The transaction itself is held with obd_chip_txn_begin().
 */
#include "uds_transport.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uds_proto.h"

static const char *TAG = "uds_manager";

/* the chip's reply: a multi-frame DID read is hundreds of bytes, a 4 KB
 * TransferData answer the worst case (hex + spaces + line prefixes) —
 * PSRAM, not the 4 KB httpd worker stack; one transaction at a time (the
 * caller's chip hold) */
#define UDS_AT_RBUF 8192
EXT_RAM_BSS_ATTR static char s_rbuf[UDS_AT_RBUF];

/* one AT line on the chip: 64 request bytes (the route caps there too) */
#define UDS_AT_REQ_MAX 64

/* the bitrate the target setup pins: 500 kbit/s unless the caller measured
   the bus at 250 (uds_at_set_can_kbps) */
static uint16_t s_can_kbps = 500;

void uds_at_set_can_kbps(uint16_t kbps)
{
    s_can_kbps = (kbps == 250) ? 250 : 500;
}

char uds_at_protocol(bool ext_id)
{
    /* ISO 15765-4: 6 = 11-bit 500k, 7 = 29-bit 500k, 8 = 11-bit 250k,
       9 = 29-bit 250k */
    if (s_can_kbps == 250)
    {
        return ext_id ? '9' : '8';
    }

    return ext_id ? '7' : '6';
}

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

uint8_t uds_at_lines_for(size_t total)
{
    /* first frame = 6 payload bytes, every consecutive frame 7 */
    if (total <= 7)
    {
        return 1;
    }

    size_t lines = 1 + (total - 6 + 6) / 7;

    return (lines > 255) ? 255 : (uint8_t)lines;
}

char uds_at_first_digit(uint8_t sid)
{
    /* "1" hands a single-frame answer over at once, and a longer answer is
     * cut and asked for AGAIN. Services that change the server's state and
     * may answer in more than one frame must not go out twice (a second
     * requestSeed, a second routine start): no digit, the chip prints every
     * frame and waits for more (bench 2026-10-02: +85 ms at ATST32, 7F xx
     * 78 lines are ridden out the same way). */
    switch (sid)
    {
    case 0x27: /* SecurityAccess          */
    case 0x29: /* Authentication          */
    case 0x31: /* RoutineControl          */
    case 0x38: /* RequestFileTransfer     */
    case 0x84: /* SecuredDataTransmission */
        return '\0';

    default:
        return '1';
    }
}

/** The ISO-TP total of a multi-frame length line ("014"), 0 = not one. */
static size_t length_line_value(const char *p, size_t len)
{
    size_t v = 0;

    if (len == 0 || len > 4)
    {
        return 0;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)p[i]))
        {
            return 0;
        }

        char c = (char)toupper((unsigned char)p[i]);

        v = (v << 4) | (size_t)((c >= 'A') ? (c - 'A' + 10) : (c - '0'));
    }

    return v;
}

bool uds_at_parse_response_len(const char *resp, uint8_t *out, size_t cap,
                               size_t *out_len, uint8_t *pending_out,
                               size_t *announced_out)
{
    if (announced_out != NULL)
    {
        *announced_out = 0;
    }

    if (resp == NULL || out == NULL || out_len == NULL)
    {
        return false;
    }

    *out_len = 0;

    if (pending_out != NULL)
    {
        *pending_out = 0;
    }

    /* The chip prints one MESSAGE per line: a single frame is one line of
     * bytes; a multi-frame (ISO-TP) message is a hex length line followed
     * by indexed lines ("0: ..", "1: .."). With the response-count digit
     * the chip still prints every '7F xx 78' responsePending it saw before
     * the final answer (bench 2026-09-16, ELM327 v2.3 core): those are
     * counted and dropped, the LAST remaining message is the answer.
     * The length line is the message's size: the last frame's padding is
     * cut off, and fewer bytes than announced means the chip stopped
     * printing early (the response-count digit counts LINES; bench
     * 2026-10-02: every multi-frame answer came back as its first 6
     * bytes and was handed over as complete). */
    size_t n = 0;          /* bytes of the message being collected        */
    bool have = false;     /* a complete non-pending message sits in out  */
    bool in_multi = false; /* collecting indexed lines                    */
    size_t total = 0;      /* announced size of the multi-frame message   */
    size_t announced = 0;  /* ... of the message that sits in out         */
    uint8_t pending = 0;

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

            if (line_has_index(p, len))
            {
                /* a frame of the current multi-frame message */
                if (!in_multi)
                {
                    in_multi = true; /* (no length line seen: tolerate) */
                    n = 0;
                    have = false;
                    total = 0;
                }

                if (!collect_line_bytes(p, len, out, cap, &n))
                {
                    return false;
                }
            }
            else
            {
                /* look ahead: is the next non-empty line indexed? then this
                   line is a multi-frame length prefix, not a message */
                const char *q = (*eol == '\0') ? eol : eol + 1;

                while (*q == '\r' || *q == '\n' || *q == ' ') q++;

                const char *qe = q;

                while (*qe != '\0' && *qe != '\r' && *qe != '\n') qe++;

                bool next_indexed = line_has_index(q, (size_t)(qe - q));

                /* a finished multi-frame message before this line */
                if (in_multi)
                {
                    in_multi = false;

                    if (total > 0 && n > total)
                    {
                        n = total; /* the last frame's padding */
                    }

                    have = (n > 0);
                    announced = total;
                }

                if (next_indexed)
                {
                    in_multi = true;
                    n = 0;
                    have = false;
                    total = length_line_value(p, len);
                }
                else
                {
                    /* one single-frame message on this line */
                    size_t m = 0;
                    uint8_t sf[8];

                    if (!collect_line_bytes(p, len, sf, sizeof(sf), &m))
                    {
                        return false;
                    }

                    if (m == 3 && sf[0] == 0x7F && sf[2] == 0x78)
                    {
                        if (pending < 255) pending++; /* responsePending */
                    }
                    else if (m > 0)
                    {
                        if (m > cap)
                        {
                            return false;
                        }

                        memcpy(out, sf, m);
                        n = m;
                        have = true;
                        announced = 0; /* a single frame */
                    }
                }
            }
        }

        p = (*eol == '\0') ? eol : eol + 1;
    }

    if (in_multi)
    {
        if (total > 0 && n > total)
        {
            n = total;
        }

        have = (n > 0);
        announced = total;
    }

    if (pending_out != NULL)
    {
        *pending_out = pending;
    }

    if (!have)
    {
        n = 0;
        announced = 0;
    }

    if (announced_out != NULL)
    {
        *announced_out = announced;
    }

    *out_len = n;
    return have && n > 0;
}

bool uds_at_parse_response_ex(const char *resp, uint8_t *out, size_t cap,
                              size_t *out_len, uint8_t *pending_out)
{
    size_t announced = 0;

    if (!uds_at_parse_response_len(resp, out, cap, out_len, pending_out,
                                   &announced))
    {
        return false;
    }

    if (announced > *out_len)
    {
        *out_len = 0;   /* cut short: never a message */
        return false;
    }

    return true;
}

bool uds_at_parse_response(const char *resp, uint8_t *out, size_t cap,
                           size_t *out_len)
{
    return uds_at_parse_response_ex(resp, out, cap, out_len, NULL);
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

esp_err_t uds_at_transceive(uds_at_request_fn req_fn,
                            const uds_addr_t *addr,
                            const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len,
                            uint32_t p2_ms, uint32_t p2star_ms,
                            uint8_t *pending_out)
{
    return uds_at_transceive_ex(req_fn, addr, req, req_len, resp, resp_cap,
                                resp_len, p2_ms, p2star_ms, pending_out,
                                false);
}

esp_err_t uds_at_transceive_ex(uds_at_request_fn req_fn,
                               const uds_addr_t *addr,
                               const uint8_t *req, size_t req_len,
                               uint8_t *resp, size_t resp_cap,
                               size_t *resp_len, uint32_t p2_ms,
                               uint32_t p2star_ms, uint8_t *pending_out,
                               bool skip_setup)
{
    if (req_fn == NULL || addr == NULL || req == NULL || req_len == 0 ||
        resp == NULL || resp_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req_len > UDS_AT_REQ_MAX)
    {
        ESP_LOGW(TAG, "request of %u B: the chip takes one AT line of %d B "
                      "(use the native ISO-TP path for larger PDUs)",
                 (unsigned)req_len, UDS_AT_REQ_MAX);
        return ESP_ERR_INVALID_ARG; /* not silently truncated */
    }

    if (pending_out != NULL)
    {
        *pending_out = 0; /* counted from the chip's lines below */
    }

    /* give the chip the extended window: it waits out responsePending
     * itself, so use p2* (fall back to p2) as the request timeout */
    uint32_t timeout_ms = (p2star_ms > p2_ms) ? p2star_ms : p2_ms;

    char at[48];
    char *rbuf = s_rbuf;

    /* Target setup (8 AT round-trips): protocol, tx header, rx filter,
     * headers-off + auto-formatting + spaces, and the chip's reply wait.
     * Skipped when the caller knows the chip is still in this setup. */
    if (!skip_setup)
    {
        /* CAN protocol: ISO 15765-4, 11-bit or 29-bit, at the bitrate
           the bus runs at (6/7 at 500k, 8/9 at 250k: a request pinned to
           the other bitrate destroys a live bus's traffic) — ATTP (try
           protocol, RAM) not ATSP, which writes the chip's EEPROM on
           every request (the driver's guard would rewrite it anyway;
           obd_chip_guard.h) */
        snprintf(at, sizeof(at), "ATTP%c", uds_at_protocol(addr->ext_id));
        (void)req_fn(at, rbuf, UDS_AT_RBUF, 800);

        if (addr->ext_id)
        {
            snprintf(at, sizeof(at), "ATCP%02lX",
                     (unsigned long)((addr->tx_id >> 24) & 0x1F));
            (void)req_fn(at, rbuf, UDS_AT_RBUF, 800);
            snprintf(at, sizeof(at), "ATSH%06lX",
                     (unsigned long)(addr->tx_id & 0xFFFFFF));
            (void)req_fn(at, rbuf, UDS_AT_RBUF, 800);
            snprintf(at, sizeof(at), "ATCRA%08lX",
                     (unsigned long)addr->rx_id);
        }
        else
        {
            snprintf(at, sizeof(at), "ATSH%03lX",
                     (unsigned long)(addr->tx_id & 0x7FF));
            (void)req_fn(at, rbuf, UDS_AT_RBUF, 800);
            snprintf(at, sizeof(at), "ATCRA%03lX",
                     (unsigned long)(addr->rx_id & 0x7FF));
        }

        (void)req_fn(at, rbuf, UDS_AT_RBUF, 800);

        /* headers off, ISO-TP auto-formatting on, spaces on (parseable) */
        (void)req_fn("ATH0", rbuf, UDS_AT_RBUF, 800);
        (void)req_fn("ATCAF1", rbuf, UDS_AT_RBUF, 800);
        (void)req_fn("ATS1", rbuf, UDS_AT_RBUF, 800);

        /* the chip's own wait for the first reply frame: ATST in 4 ms
           units from p2 (floor 40 ms, cap 1.02 s) so a single-frame answer
           comes back in ~p2 instead of the chip's 0.5 s default. 0x78
           responsePending stays chip-bound: the MIC rides it out itself
           and P2* beyond ~1 s is not reachable on this path — the native
           ISO-TP path is the answer for slow ECUs. RAM-only (no EEPROM). */
        uint32_t st = p2_ms / 4;

        if (st < 0x0A)
        {
            st = 0x0A;
        }

        if (st > 0xFF)
        {
            st = 0xFF;
        }

        snprintf(at, sizeof(at), "ATST%02lX", (unsigned long)st);
        (void)req_fn(at, rbuf, UDS_AT_RBUF, 800);
    }

    /* the request */
    char cmd[2 * UDS_AT_REQ_MAX + 2]; /* + the response-count digit */

    hex_no_space(req, req_len, cmd, sizeof(cmd));

    /* the ELM response-count digit: ONE response expected, so the chip
       hands it over as soon as it is complete instead of waiting out
       ATST for more ECUs (bench 2026-09-16: 4-8 ms vs 180-260 ms). The
       chip still rides out 7F xx 78 responsePending before the final
       answer and prints every one of them — the parser drops + counts
       them. The digit counts printed LINES, so "1" cuts a multi-frame
       answer after its first frame: the length line says how long the
       message is, and a cut answer is asked for again with the digit
       its line count needs (none above 15 lines; bench 2026-10-02:
       7 ms for the cut answer + 48 ms for a 3-frame one, against 132 ms
       without a digit). Services that must not go out twice start
       without the digit (uds_at_first_digit). */
    size_t cmd_len = strlen(cmd);
    char digit = uds_at_first_digit(req[0]);
    bool repeatable = (digit != '\0');

    /* Re-send the request (setup stays, held by the caller's transaction)
       up to 3 times on a NO-DATA / unparseable answer. The first attempt
       after grabbing the chip can catch the tail of another requester's
       ISO-TP conversation still settling on the bus; a re-send lands in a
       quiet window because the caller holds the chip across the retries
       (bench 2026-09-16: 3E 00 got NO DATA under autopid's multiframe
       load, clean on the re-send). A hard req_fn error (timeout) is the
       chip's own wait and is not retried here. */
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        cmd[cmd_len] = digit;
        cmd[cmd_len + ((digit != '\0') ? 1 : 0)] = '\0';

        err = req_fn(cmd, rbuf, UDS_AT_RBUF, timeout_ms);

        if (err != ESP_OK)
        {
            return err;
        }

        uint8_t pend = 0;
        size_t announced = 0;

        if (uds_at_parse_response_len(rbuf, resp, resp_cap, resp_len, &pend,
                                      &announced))
        {
            if (announced > *resp_len)
            {
                /* cut short: never handed over as a message */
                size_t got = *resp_len;

                *resp_len = 0;
                err = ESP_ERR_INVALID_SIZE;

                if (!repeatable)
                {
                    ESP_LOGW(TAG, "answer to %02X cut at %u of %u B, not "
                                  "asked again (the service is not "
                                  "repeatable)",
                             req[0], (unsigned)got, (unsigned)announced);
                    break;
                }

                /* ask for every line this time */
                uint8_t lines = uds_at_lines_for(announced);

                digit = (lines <= 15) ? "0123456789ABCDEF"[lines] : '\0';
                continue;
            }

            if (pending_out != NULL)
            {
                *pending_out = pend; /* the 7F xx 78 lines the chip rode out */
            }

            return ESP_OK;
        }

        err = ESP_ERR_INVALID_RESPONSE;
        vTaskDelay(pdMS_TO_TICKS(30)); /* let the bus settle, then re-send */
    }

    return err;
}
