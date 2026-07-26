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
 * @file translator_gvret_codec.c
 * @brief PURE SavvyCAN GVRET binary codec as a bridge_translator_t. Ported
 *        from legacy/gvret.c, made ctx-only + host-testable, with protocol
 *        replies delivered via the bridge reply channel (wants_reply). See
 *        bridge_manager/DESIGN_translators.md §5/§6.
 *
 *   decode : CAN-wire chunk → GVRET frame record  F1 00 <ts:4> <id:4> <dlc>
 *            <data> <xor-chk>   (id bit31 = extended)
 *   encode : GVRET command stream (F1-prefixed) → CAN TX frames (to the sink /
 *            far endpoint) + handshake replies (to hdr->reply / the client)
 *
 * Bus params in replies are advertised as 500 kbit/s enabled (the bus is
 * really owned by can_manager settings; SETUP_CANBUS is parsed + absorbed).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bridge_manager.h"
#include "can_frame_wire.h"

/* GVRET command bytes (subset we act on) */
enum { PROTO_BUILD_FRAME = 0, PROTO_TIME_SYNC = 1, PROTO_DIG_IN = 2,
       PROTO_ANA_IN = 3, PROTO_SET_DIG_OUT = 4, PROTO_SETUP_CANBUS = 5,
       PROTO_GET_CANBUS = 6, PROTO_GET_DEV_INFO = 7, PROTO_SET_SW_MODE = 8,
       PROTO_KEEPALIVE = 9, PROTO_SET_SYSTYPE = 10, PROTO_ECHO_FRAME = 11,
       PROTO_GET_NUMBUSES = 12, PROTO_GET_EXT_BUSES = 13, PROTO_SET_EXT_BUSES = 14 };

/* parser states */
enum { S_IDLE = 0, S_CMD, S_BUILD, S_SETUP_CANBUS, S_ECHO, S_ABSORB };

#define GVRET_BUILD_NUM 618
#define GVRET_BUS_SPEED 500000u

typedef struct
{
    bridge_reply_hdr_t reply;   /* filled by the pump (wants_reply)          */
    uint8_t  state;
    uint8_t  cmd;
    uint16_t step;
    uint8_t  absorb_left;       /* bytes to swallow for absorbed commands    */
    can_core_frame_t frame;
    uint32_t ts;                /* monotonic-ish timestamp for decode records*/
} gvret_ctx_t;

/* ---- ctx_init ---------------------------------------------------------- */

static esp_err_t gvret_ctx_init(void *ctx)
{
    memset(ctx, 0, sizeof(gvret_ctx_t));
    return ESP_OK;
}

/* ---- decode: CAN-wire chunk → GVRET frame record ----------------------- */

static esp_err_t gvret_emit_one(gvret_ctx_t *c, const can_core_frame_t *fp,
                                bridge_sink_fn_t sink, void *sink_arg);

static esp_err_t gvret_decode(void *vctx, const uint8_t *in, size_t len,
                              bridge_sink_fn_t sink, void *sink_arg)
{
    can_core_frame_t f;
    size_t off = 0;
    size_t consumed;

    /* chunks may be COALESCED (several wire frames per chunk) */
    while ((consumed = can_wire_decode_next(in + off, len - off, &f)) > 0)
    {
        esp_err_t err = gvret_emit_one(vctx, &f, sink, sink_arg);

        if (err != ESP_OK)
        {
            return err;
        }

        off += consumed;
    }

    return ESP_OK;
}

static esp_err_t gvret_emit_one(gvret_ctx_t *c, const can_core_frame_t *fp,
                                bridge_sink_fn_t sink, void *sink_arg)
{
    const can_core_frame_t f = *fp;
    uint8_t buf[32];
    int n = 0;
    uint32_t id, ts;
    uint8_t chk = 0;

    id = f.id;
    if (f.ext) id |= (1u << 31);
    ts = c->ts;
    c->ts += 1000; /* relative µs; SavvyCAN uses deltas */

    buf[n++] = 0xF1;
    buf[n++] = 0x00;
    buf[n++] = (uint8_t)(ts);
    buf[n++] = (uint8_t)(ts >> 8);
    buf[n++] = (uint8_t)(ts >> 16);
    buf[n++] = (uint8_t)(ts >> 24);
    buf[n++] = (uint8_t)(id);
    buf[n++] = (uint8_t)(id >> 8);
    buf[n++] = (uint8_t)(id >> 16);
    buf[n++] = (uint8_t)(id >> 24);
    buf[n++] = f.dlc;
    for (int i = 0; i < f.dlc && i < 8; i++)
    {
        buf[n++] = f.data[i];
    }

    for (int i = 0; i < n; i++) chk ^= buf[i]; /* XOR over 11+dlc bytes */
    buf[n++] = chk;

    return sink(sink_arg, buf, (size_t)n);
}

/* ---- reply helpers (to the client via the reply channel) --------------- */

static esp_err_t reply(gvret_ctx_t *c, const uint8_t *b, size_t n)
{
    if (c->reply.reply == NULL) return ESP_OK; /* no channel bound (host test) */
    return c->reply.reply(c->reply.reply_arg, b, n);
}

static esp_err_t handle_command(gvret_ctx_t *c, uint8_t cmd,
                                bridge_sink_fn_t sink, void *sink_arg)
{
    uint8_t r[24];
    (void)sink; (void)sink_arg;

    switch (cmd)
    {
    case PROTO_BUILD_FRAME:
        c->state = S_BUILD; c->step = 0;
        memset(&c->frame, 0, sizeof(c->frame));
        return ESP_OK;
    case PROTO_TIME_SYNC:
        r[0] = 0xF1; r[1] = 1;
        r[2] = (uint8_t)c->ts; r[3] = (uint8_t)(c->ts >> 8);
        r[4] = (uint8_t)(c->ts >> 16); r[5] = (uint8_t)(c->ts >> 24);
        c->state = S_IDLE; return reply(c, r, 6);
    case PROTO_DIG_IN:
        r[0] = 0xF1; r[1] = 2; r[2] = 0; r[3] = 0; /* checksum of {} */
        c->state = S_IDLE; return reply(c, r, 4);
    case PROTO_ANA_IN:
        memset(r, 0, 17); r[0] = 0xF1; r[1] = 3;
        c->state = S_IDLE; return reply(c, r, 17);
    case PROTO_GET_CANBUS:
        r[0] = 0xF1; r[1] = 6;
        r[2] = 1;                              /* enabled, not listen-only  */
        r[3] = (uint8_t)GVRET_BUS_SPEED;  r[4] = (uint8_t)(GVRET_BUS_SPEED >> 8);
        r[5] = (uint8_t)(GVRET_BUS_SPEED >> 16); r[6] = (uint8_t)(GVRET_BUS_SPEED >> 24);
        r[7] = 0; r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0; /* bus1 disabled */
        c->state = S_IDLE; return reply(c, r, 12);
    case PROTO_GET_DEV_INFO:
        r[0] = 0xF1; r[1] = 7;
        r[2] = (uint8_t)GVRET_BUILD_NUM; r[3] = (uint8_t)(GVRET_BUILD_NUM >> 8);
        r[4] = 0x20; r[5] = 0; r[6] = 0; r[7] = 0;
        c->state = S_IDLE; return reply(c, r, 8);
    case PROTO_KEEPALIVE:
        r[0] = 0xF1; r[1] = 0x09; r[2] = 0xDE; r[3] = 0xAD;
        c->state = S_IDLE; return reply(c, r, 4);
    case PROTO_GET_NUMBUSES:
        r[0] = 0xF1; r[1] = 12; r[2] = 1;
        c->state = S_IDLE; return reply(c, r, 3);
    case PROTO_GET_EXT_BUSES:
        memset(r, 0, 17); r[0] = 0xF1; r[1] = 13;
        c->state = S_IDLE; return reply(c, r, 17);
    case PROTO_SETUP_CANBUS:
        c->state = S_SETUP_CANBUS; c->step = 0; return ESP_OK; /* parse+absorb 8 */
    case PROTO_ECHO_FRAME:
        c->state = S_ECHO; c->step = 0; return ESP_OK;         /* absorb a frame */
    case PROTO_SET_DIG_OUT:
    case PROTO_SET_SW_MODE:
    case PROTO_SET_SYSTYPE:
        c->state = S_ABSORB; c->absorb_left = 1; return ESP_OK;
    case PROTO_SET_EXT_BUSES:
        c->state = S_ABSORB; c->absorb_left = 12; return ESP_OK;
    default:
        c->state = S_IDLE; return ESP_OK;
    }
}

/* ---- encode: GVRET command stream → CAN TX + replies ------------------- */

static esp_err_t gvret_encode(void *vctx, const uint8_t *in, size_t len,
                              bridge_sink_fn_t sink, void *sink_arg)
{
    gvret_ctx_t *c = vctx;

    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = in[i];

        switch (c->state)
        {
        case S_IDLE:
            if (b == 0xF1) c->state = S_CMD;
            /* 0xE7 (binary mode) and console bytes are ignored */
            break;

        case S_CMD:
        {
            esp_err_t err = handle_command(c, b, sink, sink_arg);
            if (err != ESP_OK) return err;
            break;
        }

        case S_BUILD:
            switch (c->step)
            {
            case 0: c->frame.id = b; break;
            case 1: c->frame.id |= (uint32_t)b << 8; break;
            case 2: c->frame.id |= (uint32_t)b << 16; break;
            case 3:
                c->frame.id |= (uint32_t)b << 24;
                c->frame.ext = (c->frame.id & (1u << 31)) != 0;
                c->frame.id &= 0x7FFFFFFFu;
                break;
            case 4: break; /* bus */
            case 5:
                c->frame.dlc = b & 0xF;
                if (c->frame.dlc > 8) c->frame.dlc = 8;
                break;
            default:
                if (c->step < (uint16_t)(c->frame.dlc + 6))
                {
                    c->frame.data[c->step - 6] = b;
                }
                else
                {
                    /* trailing checksum byte → frame complete, TX it */
                    uint8_t wire[CAN_WIRE_MAX];
                    size_t n = can_wire_encode(&c->frame, wire);
                    c->state = S_IDLE;
                    esp_err_t err = sink(sink_arg, wire, n);
                    if (err != ESP_OK) return err;
                }
                break;
            }
            c->step++;
            break;

        case S_SETUP_CANBUS:
            /* 8 bytes: speed + status (absorbed; bus owned by can_manager) */
            if (++c->step >= 8) c->state = S_IDLE;
            break;

        case S_ECHO:
            /* id(4) bus(1) dlc(1) then dlc data + checksum — swallow; a
             * conservative fixed swallow of 6 + up-to-8 + 1 is unknown until
             * dlc, so track like BUILD but emit nothing */
            if (c->step == 5)
            {
                c->absorb_left = (uint8_t)((in[i] & 0xF) > 8 ? 8 : (in[i] & 0xF));
                c->absorb_left += 1; /* checksum */
                c->step++;
            }
            else if (c->step > 5)
            {
                if (--c->absorb_left == 0) c->state = S_IDLE;
                c->step++;
            }
            else
            {
                c->step++;
            }
            break;

        case S_ABSORB:
            if (--c->absorb_left == 0) c->state = S_IDLE;
            break;

        default:
            c->state = S_IDLE;
            break;
        }
    }

    return ESP_OK;
}

const bridge_translator_t translator_gvret_desc =
{
    .name        = "gvret",
    .ctx_size    = sizeof(gvret_ctx_t),
    .wants_reply = true,
    .ctx_init    = gvret_ctx_init,
    .decode      = gvret_decode,
    .encode      = gvret_encode,
};
