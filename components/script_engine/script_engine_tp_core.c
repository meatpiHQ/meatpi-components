/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_tp_core.c
 * @brief ISO 15765-2 frames for the flash bindings (pure; see the header).
 */
#include "script_engine_tp_core.h"

#include <string.h>

void se_tp_opts_default(se_tp_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->ae_tx = -1;
    o->ae_rx = -1;
    o->pad = true;
}

/* Start a frame: the AE byte if any; returns the PCI position. */
static size_t frame_start(const se_tp_opts_t *o, uint8_t out[8])
{
    memset(out, o->pad_byte, 8);

    if (o->ae_tx >= 0)
    {
        out[0] = (uint8_t)o->ae_tx;
        return 1;
    }

    return 0;
}

static uint8_t frame_end(const se_tp_opts_t *o, size_t used)
{
    return o->pad ? 8 : (uint8_t)used;
}

uint8_t se_tp_first(const se_tp_opts_t *o, const uint8_t *msg, size_t len,
                    uint8_t out[8], size_t *consumed)
{
    *consumed = 0;

    if (len == 0 || len > SE_TP_MAX || msg == NULL)
    {
        return 0;
    }

    size_t p = frame_start(o, out);
    size_t sf_room = 7 - p;

    if (len <= sf_room)
    {
        out[p] = (uint8_t)len;
        memcpy(&out[p + 1], msg, len);
        *consumed = len;
        return frame_end(o, p + 1 + len);
    }

    out[p] = (uint8_t)(0x10 | ((len >> 8) & 0x0F));
    out[p + 1] = (uint8_t)(len & 0xFF);

    size_t n = 8 - (p + 2);

    memcpy(&out[p + 2], msg, n);
    *consumed = n;
    return 8;
}

uint8_t se_tp_consecutive(const se_tp_opts_t *o, const uint8_t *msg,
                          size_t len, size_t *off, uint8_t sn,
                          uint8_t out[8])
{
    if (*off >= len)
    {
        return 0;
    }

    size_t p = frame_start(o, out);
    size_t room = 7 - p;
    size_t n = len - *off;

    if (n > room)
    {
        n = room;
    }

    out[p] = (uint8_t)(0x20 | (sn & 0x0F));
    memcpy(&out[p + 1], msg + *off, n);
    *off += n;
    return frame_end(o, p + 1 + n);
}

uint8_t se_tp_flow_control(const se_tp_opts_t *o, uint8_t out[8])
{
    size_t p = frame_start(o, out);

    out[p] = 0x30;
    out[p + 1] = o->bs;
    out[p + 2] = o->stmin;
    return frame_end(o, p + 3);
}

uint32_t se_tp_stmin_us(uint8_t stmin)
{
    if (stmin <= 0x7F)
    {
        return (uint32_t)stmin * 1000U;
    }

    if (stmin >= 0xF1 && stmin <= 0xF9)
    {
        return (uint32_t)(stmin - 0xF0) * 100U;
    }

    return 127000U; /* reserved: the longest legal value (ISO 15765-2) */
}

/* The PCI position of a received frame, or -1 when it isn't ours. */
static int rx_pci(const se_tp_opts_t *o, const uint8_t *d, uint8_t dlc)
{
    if (o->ae_rx >= 0)
    {
        if (dlc < 2 || d[0] != (uint8_t)o->ae_rx)
        {
            return -1;
        }

        return 1;
    }

    return (dlc < 1) ? -1 : 0;
}

se_tp_fc_t se_tp_parse_fc(const se_tp_opts_t *o, const uint8_t *d,
                          uint8_t dlc, uint8_t *bs, uint32_t *stmin_us)
{
    int p = rx_pci(o, d, dlc);

    if (p < 0 || (d[p] & 0xF0) != 0x30 || dlc < (uint8_t)(p + 3))
    {
        return SE_TP_FC_NOT_FC;
    }

    switch (d[p] & 0x0F)
    {
        case 0:
            *bs = d[p + 1];
            *stmin_us = se_tp_stmin_us(d[p + 2]);
            return SE_TP_FC_CTS;
        case 1:
            return SE_TP_FC_WAIT;
        case 2:
            return SE_TP_FC_OVERFLOW;
        default:
            return SE_TP_FC_NOT_FC;
    }
}

void se_tp_rx_init(se_tp_rx_t *rx, uint8_t *buf, size_t cap)
{
    memset(rx, 0, sizeof(*rx));
    rx->buf = buf;
    rx->cap = cap;
}

static se_tp_rx_status_t rx_fail(se_tp_rx_t *rx)
{
    rx->active = false;
    rx->len = 0;
    rx->got = 0;
    return SE_TP_RX_ERROR;
}

se_tp_rx_status_t se_tp_rx_feed(se_tp_rx_t *rx, const se_tp_opts_t *o,
                                const uint8_t *d, uint8_t dlc)
{
    int p = rx_pci(o, d, dlc);

    if (p < 0)
    {
        return SE_TP_RX_IGNORED;
    }

    uint8_t pci = d[p] & 0xF0;
    size_t avail = dlc - (size_t)p - 1;

    if (pci == 0x00)
    {
        size_t n = d[p] & 0x0F;

        if (n == 0 || n > avail || n > rx->cap)
        {
            return rx_fail(rx);
        }

        memcpy(rx->buf, &d[p + 1], n);
        rx->len = n;
        rx->got = n;
        rx->active = false;
        return SE_TP_RX_DONE;
    }

    if (pci == 0x10)
    {
        if (dlc < (uint8_t)(p + 2))
        {
            return rx_fail(rx);
        }

        size_t n = ((size_t)(d[p] & 0x0F) << 8) | d[p + 1];
        size_t first = dlc - (size_t)p - 2;

        if (n <= first || n > rx->cap)
        {
            return rx_fail(rx);
        }

        memcpy(rx->buf, &d[p + 2], first);
        rx->len = n;
        rx->got = first;
        rx->next_sn = 1;
        rx->blk = 0;
        rx->active = true;
        return SE_TP_RX_NEED_FC;
    }

    if (pci == 0x20)
    {
        if (!rx->active)
        {
            return SE_TP_RX_IGNORED; /* a stray CF: not part of ours */
        }

        if ((d[p] & 0x0F) != rx->next_sn)
        {
            return rx_fail(rx);
        }

        size_t n = rx->len - rx->got;

        if (n > avail)
        {
            n = avail;
        }

        memcpy(rx->buf + rx->got, &d[p + 1], n);
        rx->got += n;
        rx->next_sn = (uint8_t)((rx->next_sn + 1) & 0x0F);

        if (rx->got >= rx->len)
        {
            rx->active = false;
            return SE_TP_RX_DONE;
        }

        /* a block of o->bs CFs: the sender waits for our next FC */
        if (o->bs > 0 && ++rx->blk >= o->bs)
        {
            rx->blk = 0;
            return SE_TP_RX_NEED_FC;
        }

        return SE_TP_RX_MORE;
    }

    if (pci == 0x30)
    {
        return SE_TP_RX_IGNORED; /* a flow control: the send path's */
    }

    return rx_fail(rx);
}
