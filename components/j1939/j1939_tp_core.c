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
 * @file j1939_tp_core.c
 * @brief J1939-21 transport protocol reassembly for a listener, and the
 *        receiving end of a connection addressed to this node. Pure.
 *
 * The books balance: every announce is counted (started, no_session or
 * bad_cm), every started session ends as exactly one of completed,
 * seq_errors, timeouts, aborted or replaced, and a data packet that belongs
 * to no session is an orphan_dt.
 */
#include "j1939_tp_core.h"

#include <string.h>

#define TP_PACKET_BYTES 7u
#define TP_MIN_BYTES    9u /* up to 8 bytes travel in one frame */

/* ---- replies this node sends (destination role) -------------------------------- */

static void pgn_put(uint32_t pgn, uint8_t *d)
{
    d[0] = (uint8_t)pgn;
    d[1] = (uint8_t)(pgn >> 8);
    d[2] = (uint8_t)((pgn >> 16) & 0x03u);
}

static void reply_queue(j1939_tp_t *tp, uint8_t da, const uint8_t data[8])
{
    if (tp->reply_n >= J1939_TP_REPLIES)
    {
        tp->stats.reply_lost++;
        return;
    }

    tp->reply[tp->reply_n].da = da;
    memcpy(tp->reply[tp->reply_n].data, data, 8);
    tp->reply_n++;
}

/** Clear to send: the next window of a session addressed to this node. */
static void reply_cts(j1939_tp_t *tp, j1939_tp_session_t *s)
{
    uint8_t d[8] = { J1939_TP_CTRL_CTS, 0, 0, 0xFF, 0xFF, 0, 0, 0 };
    unsigned left = (unsigned)s->packets - (unsigned)s->next + 1u;

    s->cts_left = (uint8_t)((left < s->cts_max) ? left : s->cts_max);
    d[1] = s->cts_left;
    d[2] = s->next;
    pgn_put(s->pgn, &d[5]);
    tp->stats.cts++;
    reply_queue(tp, s->sa, d);
}

static void reply_eoma(j1939_tp_t *tp, const j1939_tp_session_t *s)
{
    uint8_t d[8] = { J1939_TP_CTRL_EOMA, (uint8_t)s->size,
                     (uint8_t)(s->size >> 8), s->packets, 0xFF, 0, 0, 0 };

    pgn_put(s->pgn, &d[5]);
    tp->stats.eoma++;
    reply_queue(tp, s->sa, d);
}

static void reply_abort(j1939_tp_t *tp, uint8_t da, uint32_t pgn,
                        uint8_t reason)
{
    uint8_t d[8] = { J1939_TP_CTRL_ABORT, reason, 0xFF, 0xFF, 0xFF, 0, 0, 0 };

    pgn_put(pgn, &d[5]);
    tp->stats.aborts_out++;
    reply_queue(tp, da, d);
}

/** A request to send is for this node when it holds an address and the
 *  frame names it. */
static bool for_me(const j1939_tp_t *tp, uint8_t da)
{
    return tp->my_sa != J1939_ADDR_NULL && da == tp->my_sa;
}

static j1939_tp_session_t *session_find(j1939_tp_t *tp, uint8_t sa, uint8_t da)
{
    for (size_t i = 0; i < J1939_TP_SESSIONS; i++)
    {
        if (tp->s[i].open && tp->s[i].sa == sa && tp->s[i].da == da)
        {
            return &tp->s[i];
        }
    }

    return NULL;
}

static j1939_tp_session_t *session_free(j1939_tp_t *tp)
{
    for (size_t i = 0; i < J1939_TP_SESSIONS; i++)
    {
        if (!tp->s[i].open)
        {
            return &tp->s[i];
        }
    }

    return NULL;
}

static uint32_t cm_pgn(const uint8_t *d)
{
    return (uint32_t)d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16);
}

/** BAM or RTS: a message of d[1..2] bytes in d[3] packets is coming. */
static void announce(j1939_tp_t *tp, uint8_t sa, uint8_t da, const uint8_t *d,
                     int64_t now_us)
{
    uint16_t size = (uint16_t)(d[1] | ((uint16_t)d[2] << 8));
    uint8_t packets = d[3];
    bool mine = (d[0] == J1939_TP_CTRL_RTS) && for_me(tp, da);

    if (size < TP_MIN_BYTES || size > J1939_MSG_MAX ||
        packets != (size + TP_PACKET_BYTES - 1) / TP_PACKET_BYTES)
    {
        tp->stats.bad_cm++;
        return;
    }

    j1939_tp_session_t *s = session_find(tp, sa, da);

    if (s != NULL)
    {
        tp->stats.replaced++; /* the sender gave up on the open one */
    }
    else
    {
        s = session_free(tp);

        if (s == NULL)
        {
            tp->stats.no_session++;

            if (mine)
            {
                /* the sender is waiting for us: tell it, do not let it
                   time out */
                reply_abort(tp, sa, cm_pgn(d), J1939_TP_ABORT_BUSY);
            }

            return;
        }
    }

    s->open = true;
    s->mine = mine;
    s->sa = sa;
    s->da = da;
    s->packets = packets;
    s->next = 1;
    s->size = size;
    s->pgn = cm_pgn(d);
    s->last_us = now_us;
    /* RTS byte 4: packets the sender takes per clear-to-send (FF: any) */
    s->cts_max = (mine && d[4] != 0) ? d[4] : 0xFF;
    s->cts_left = 0;
    tp->stats.started++;

    if (mine)
    {
        tp->stats.to_me++;
        reply_cts(tp, s);
    }
}

void j1939_tp_init(j1939_tp_t *tp)
{
    memset(tp->s, 0, sizeof(tp->s));
    memset(&tp->stats, 0, sizeof(tp->stats));
    tp->my_sa = J1939_ADDR_NULL;
    tp->reply_n = 0;
}

void j1939_tp_set_address(j1939_tp_t *tp, uint8_t my_sa)
{
    tp->my_sa = my_sa;
}

bool j1939_tp_reply_take(j1939_tp_t *tp, j1939_tp_reply_t *out)
{
    if (tp->reply_n == 0)
    {
        return false;
    }

    if (out != NULL)
    {
        *out = tp->reply[0];
    }

    tp->reply_n--;
    memmove(&tp->reply[0], &tp->reply[1], tp->reply_n * sizeof(tp->reply[0]));
    return true;
}

void j1939_tp_cm(j1939_tp_t *tp, uint8_t sa, uint8_t da, const uint8_t *d,
                 uint8_t dlc, int64_t now_us)
{
    j1939_tp_session_t *s;

    if (dlc != 8)
    {
        tp->stats.bad_cm++;
        return;
    }

    switch (d[0])
    {
    case J1939_TP_CTRL_BAM:
        if (da != J1939_ADDR_GLOBAL)
        {
            tp->stats.bad_cm++;
            return;
        }

        announce(tp, sa, da, d, now_us);
        return;

    case J1939_TP_CTRL_RTS:
        if (da == J1939_ADDR_GLOBAL)
        {
            tp->stats.bad_cm++;
            return;
        }

        announce(tp, sa, da, d, now_us);
        return;

    case J1939_TP_CTRL_CTS:
        /* from the receiver: the session is (frame's da -> frame's sa) */
        s = session_find(tp, da, sa);

        if (s == NULL || s->pgn != cm_pgn(d))
        {
            return; /* a connection this listener never saw open */
        }

        s->last_us = now_us;

        if (d[1] == 0)
        {
            return; /* hold: the receiver asks the sender to wait */
        }

        if (d[2] >= 1 && d[2] <= s->next)
        {
            s->next = d[2]; /* the receiver wants these (again) */
        }
        else
        {
            /* it is ahead of this listener: packets were lost here */
            s->open = false;
            tp->stats.seq_errors++;
        }

        return;

    case J1939_TP_CTRL_EOMA:
        s = session_find(tp, da, sa);

        if (s != NULL && s->pgn == cm_pgn(d))
        {
            /* the receiver has it all and this listener does not */
            s->open = false;
            tp->stats.seq_errors++;
        }

        return;

    case J1939_TP_CTRL_ABORT:
        s = session_find(tp, sa, da);

        if (s == NULL)
        {
            s = session_find(tp, da, sa);
        }

        if (s != NULL && s->pgn == cm_pgn(d))
        {
            s->open = false;
            tp->stats.aborted++;
        }

        return;

    default:
        tp->stats.bad_cm++;
        return;
    }
}

bool j1939_tp_dt(j1939_tp_t *tp, uint8_t sa, uint8_t da, const uint8_t *d,
                 uint8_t dlc, int64_t now_us, j1939_tp_msg_t *out)
{
    j1939_tp_session_t *s = session_find(tp, sa, da);

    if (s == NULL)
    {
        tp->stats.orphan_dt++;
        return false;
    }

    if (dlc != 8 || d[0] != s->next)
    {
        /* a gap cannot be filled by listening: the message is lost. As the
           destination we could ask for the packet again; a sender that
           skips one is broken, so the connection is aborted instead */
        s->open = false;
        tp->stats.seq_errors++;

        if (s->mine)
        {
            reply_abort(tp, s->sa, s->pgn, J1939_TP_ABORT_SEQUENCE);
        }

        return false;
    }

    size_t idx = (size_t)(s - tp->s);
    size_t off = (size_t)(s->next - 1u) * TP_PACKET_BYTES;
    size_t n = s->size - off;

    if (n > TP_PACKET_BYTES)
    {
        n = TP_PACKET_BYTES;
    }

    memcpy(&tp->buf[idx][off], &d[1], n);
    s->last_us = now_us;

    if (s->next < s->packets)
    {
        s->next++;

        if (s->mine && s->cts_left > 0 && --s->cts_left == 0)
        {
            reply_cts(tp, s); /* the window is used up: the next one */
        }

        return false;
    }

    s->open = false;
    tp->stats.completed++;

    if (s->mine)
    {
        reply_eoma(tp, s);
    }

    if (out != NULL)
    {
        out->pgn = s->pgn;
        out->sa = s->sa;
        out->da = s->da;
        out->len = s->size;
        out->data = tp->buf[idx];
    }

    return true;
}

void j1939_tp_expire(j1939_tp_t *tp, int64_t now_us)
{
    for (size_t i = 0; i < J1939_TP_SESSIONS; i++)
    {
        if (tp->s[i].open && now_us - tp->s[i].last_us > J1939_TP_TIMEOUT_US)
        {
            tp->s[i].open = false;
            tp->stats.timeouts++;

            if (tp->s[i].mine)
            {
                reply_abort(tp, tp->s[i].sa, tp->s[i].pgn,
                            J1939_TP_ABORT_TIMEOUT);
            }
        }
    }
}

size_t j1939_tp_open(const j1939_tp_t *tp)
{
    size_t n = 0;

    for (size_t i = 0; i < J1939_TP_SESSIONS; i++)
    {
        if (tp->s[i].open)
        {
            n++;
        }
    }

    return n;
}
