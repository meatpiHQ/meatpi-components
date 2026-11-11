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
 * @file autopid_resp_precan.c
 * @brief PURE: a reply of a pre-CAN protocol (SAE J1850, ISO 9141-2, ISO
 *        14230-4) as the payloads the rest of autopid reads (host-tested).
 *
 * Such a reply has no CAN id and no ISO-TP. With headers on the chip prints
 * every message as its header, its data and a checksum (bench 2026-11-11 and
 * 2026-11-12, the MIC3624 against ECU sim v2; the 4Runner's lines are those
 * of GitHub issue #1045):
 *
 *   48 6B 10 41 0C 0C 80 9C        ISO 9141-2: priority, target, the ECU,
 *                                  the data, the sum of all before it
 *   48 6B 10 41 0C 0C 80 02        J1850 VPW (41 6B 10 .. on PWM): the same
 *                                  header, the SAE J1850 CRC
 *   84 F1 10 41 0C 0C 80 5E        ISO 14230: a format byte with the data
 *                                  length, the tester, the ECU, the sum
 *   41 0C 0C 80                    any of them with headers off
 *
 * Read as a CAN reply, the eight bytes became data and the engine speed came
 * out as 15428 where it was 800 (the baseline of 2026-11-11). Here a message
 * becomes what a CAN ECU would have answered: the data alone as the payload,
 * the ECU's address where the CAN id sits, one entry per ECU with the
 * lowest address first (the engine, 0x10, before the transmission, 0x18).
 * ap_resp_frames() then prints the payload as the ISO-TP frame the
 * expressions index (`04 41 0C 0C 80`, so `[B3:B4]/4` is the engine speed on
 * every protocol).
 *
 * A reply in several messages is joined only where the standard splits one
 * item over them: service 09 (the VIN of an ISO 9141-2 or J1850 car comes
 * in five messages, `49 02 01 00 00 00 31`, `49 02 02 57 43 41 4E`, ..:
 * joined to `49 02 01` and the seventeen characters, as a CAN ECU sends
 * it). Trouble codes (three per message, no count) keep their own layout
 * for now: the DTC codec's turn comes with the scan on these protocols.
 *
 * The protocol in effect is a HINT the caller sets (ap_resp_set_proto_hint),
 * as the 29-bit rule's is: a line of bytes cannot tell by itself. With the
 * hint, a line counts as a message with its header when the header fits the
 * protocol, the first data byte is a response's (0x40 to 0x7F) and the
 * checksum is right; anything else is taken as data, which is what the chip
 * prints with headers off (its state after a reset).
 */
#include "autopid_private.h"
#include "autopid_proto.h"
#include "autopid_resp_private.h"

#include <string.h>

#include "esp_attr.h"

static char s_hint; /* the pre-CAN protocol in effect, '\0' = none */

void ap_resp_set_proto_hint(char proto)
{
    s_hint = ap_proto_precan(proto) ? proto : '\0';
}

char ap_resp_proto_hint(void)
{
    return s_hint;
}

typedef struct
{
    uint32_t       src;   /* the ECU's address, UINT32_MAX = headers off */
    const uint8_t *data;
    size_t         n;
} msg_t;

/** A response's first data byte: a positive answer (the service + 0x40) or
 *  the negative one (0x7F). */
static bool is_response(uint8_t sid)
{
    return (sid & 0xC0) == 0x40;
}

/** @p line as a message with its header and checksum. False = it is none. */
static bool with_header(const ap_line_t *line, ap_proto_frame_t frame,
                        msg_t *m)
{
    const uint8_t *b = line->bytes;
    size_t n = line->n;
    size_t hdr = 3;
    size_t len;

    if (frame == AP_PF_KWP_SUM)
    {
        if (n < 3)
        {
            return false;
        }

        /* the format byte: its top two bits say whether the two address
           bytes follow, its low six the data length (0 = a length byte) */
        bool addressed = (b[0] & 0xC0) != 0;

        hdr = 1 + (addressed ? 2 : 0) + (((b[0] & 0x3F) == 0) ? 1 : 0);

        if (n < hdr + 2)
        {
            return false;
        }

        len = ((b[0] & 0x3F) != 0) ? (size_t)(b[0] & 0x3F) : b[hdr - 1];
        m->src = addressed ? b[2] : 0;

        if (n != hdr + len + 1)
        {
            return false;
        }
    }
    else
    {
        if (n < 5)
        {
            return false;
        }

        len = n - 4;
        m->src = b[2];
    }

    uint8_t want = (frame == AP_PF_HDR3_CRC) ? ap_proto_crc_j1850(b, n - 1)
                                             : ap_proto_sum8(b, n - 1);

    if (len == 0 || b[n - 1] != want || !is_response(b[hdr]))
    {
        return false;
    }

    m->data = b + hdr;
    m->n = len;
    return true;
}

/** The messages of ECU @p src, in arrival order, as one payload. */
static size_t join(const msg_t *msgs, int n_msgs, uint32_t src,
                   uint8_t *out, size_t cap)
{
    size_t n = 0;
    int taken = 0;

    for (int i = 0; i < n_msgs; i++)
    {
        const msg_t *m = &msgs[i];

        if (m->src != src)
        {
            continue;
        }

        if (taken == 0)
        {
            n = (m->n < cap) ? m->n : cap;
            memcpy(out, m->data, n);
        }
        else if (out[0] == 0x49 && m->n > 3 && m->data[0] == 0x49 &&
                 m->data[1] == out[1])
        {
            /* service 09: the next piece of the same item, behind the
               service, the item and the message number */
            size_t add = m->n - 3;

            if (add > cap - n)
            {
                add = cap - n;
            }

            memcpy(out + n, m->data + 3, add);
            n += add;
        }
        else
        {
            continue; /* another answer of the same ECU: the first one stands */
        }

        taken++;
    }

    if (taken > 1 && out[0] == 0x49 && out[1] == 0x02 && n > 3 + 17)
    {
        /* the VIN: seventeen characters, filled up in front (`00 00 00 31`
           is the first message's four bytes), one item */
        memmove(out + 3, out + n - 17, 17);
        out[2] = 0x01;
        n = 3 + 17;
    }

    return n;
}

int ap_resp_precan_ecus(const ap_line_t *lines, int n_lines, long iso_total,
                        ap_resp_ecu_t *out, int max_ecus)
{
    const ap_proto_t *proto = ap_proto(s_hint);

    if (proto == NULL || n_lines <= 0 || iso_total >= 0 || max_ecus <= 0)
    {
        return 0;
    }

    /* PSRAM, under the caller's lines lock (the line table's own rule) */
    static msg_t s_msgs[AP_MAX_LINES] EXT_RAM_BSS_ATTR;
    int n_msgs = 0;
    msg_t first;
    bool headers = with_header(&lines[0], proto->frame, &first);

    for (int i = 0; i < n_lines; i++)
    {
        if (lines[i].has_header || lines[i].iso_index >= 0)
        {
            return 0; /* a CAN line after all: the hint is stale, not ours */
        }

        msg_t m = { UINT32_MAX, lines[i].bytes, lines[i].n };

        if (headers && !with_header(&lines[i], proto->frame, &m))
        {
            continue; /* a damaged message among good ones */
        }

        if (m.n > 0)
        {
            s_msgs[n_msgs++] = m;
        }
    }

    /* the distinct ECUs, ascending; over the cap the lowest stay */
    uint32_t srcs[AP_RESP_ECUS_MAX];
    int n_srcs = 0;

    if (max_ecus > AP_RESP_ECUS_MAX)
    {
        max_ecus = AP_RESP_ECUS_MAX;
    }

    for (int i = 0; i < n_msgs; i++)
    {
        uint32_t s = s_msgs[i].src;
        int at = 0;

        while (at < n_srcs && srcs[at] < s)
        {
            at++;
        }

        if ((at < n_srcs && srcs[at] == s) || at >= max_ecus)
        {
            continue;
        }

        if (n_srcs < max_ecus)
        {
            n_srcs++;
        }

        for (int k = n_srcs - 1; k > at; k--)
        {
            srcs[k] = srcs[k - 1];
        }

        srcs[at] = s;
    }

    int n_out = 0;

    for (int i = 0; i < n_srcs; i++)
    {
        size_t len = join(s_msgs, n_msgs, srcs[i], out[n_out].payload,
                          sizeof(out[n_out].payload));

        if (len > 0)
        {
            out[n_out].header = srcs[i];
            out[n_out].len = len;
            n_out++;
        }
    }

    return n_out;
}
