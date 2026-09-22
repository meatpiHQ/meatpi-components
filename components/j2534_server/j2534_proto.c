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
 * @file j2534_proto.c
 * @brief Pure J2534 wire codec + filter match (see j2534_proto.h).
 *        Little-endian on the wire, independent of host endianness.
 */
#include "j2534_proto.h"

#include <string.h>

/* ---- little-endian helpers -------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- header ----------------------------------------------------------- */

size_t j2534_hdr_encode(uint8_t *buf, uint8_t type, uint16_t seq,
                        uint16_t channel, uint32_t length)
{
    put_u16(buf + 0, J2534_MAGIC);
    buf[2] = J2534_WIRE_VERSION;
    buf[3] = type;
    put_u16(buf + 4, seq);
    put_u16(buf + 6, channel);
    put_u32(buf + 8, length);
    return J2534_HDR_SIZE;
}

bool j2534_hdr_decode(const uint8_t *buf, size_t len, j2534_hdr_t *out)
{
    if (buf == NULL || out == NULL || len < J2534_HDR_SIZE)
    {
        return false;
    }

    if (get_u16(buf + 0) != J2534_MAGIC || buf[2] != J2534_WIRE_VERSION)
    {
        return false;
    }

    out->magic   = J2534_MAGIC;
    out->version = buf[2];
    out->type    = buf[3];
    out->seq     = get_u16(buf + 4);
    out->channel = get_u16(buf + 6);
    out->length  = get_u32(buf + 8);
    return true;
}

/* ---- PASSTHRU_MSG ----------------------------------------------------- */

#define MSG_FIXED 24 /* 6 × u32 before the data bytes */

size_t j2534_msg_encode(const j2534_msg_t *msg, uint8_t *buf, size_t cap)
{
    if (msg == NULL || buf == NULL || msg->data_size > J2534_MAX_DATA)
    {
        return 0;
    }

    size_t total = MSG_FIXED + msg->data_size;

    if (cap < total)
    {
        return 0;
    }

    put_u32(buf + 0,  msg->protocol_id);
    put_u32(buf + 4,  msg->rx_status);
    put_u32(buf + 8,  msg->tx_flags);
    put_u32(buf + 12, msg->timestamp);
    put_u32(buf + 16, msg->extra_data_index);
    put_u32(buf + 20, msg->data_size);
    memcpy(buf + MSG_FIXED, msg->data, msg->data_size);
    return total;
}

bool j2534_msg_decode(const uint8_t *buf, size_t len, j2534_msg_t *out)
{
    if (buf == NULL || out == NULL || len < MSG_FIXED)
    {
        return false;
    }

    uint32_t ds = get_u32(buf + 20);

    if (ds > J2534_MAX_DATA || ds > len - MSG_FIXED)
    {
        return false;
    }

    out->protocol_id      = get_u32(buf + 0);
    out->rx_status        = get_u32(buf + 4);
    out->tx_flags         = get_u32(buf + 8);
    out->timestamp        = get_u32(buf + 12);
    out->extra_data_index = get_u32(buf + 16);
    out->data_size        = ds;
    memcpy(out->data, buf + MSG_FIXED, ds);
    return true;
}

/* ---- filter match ----------------------------------------------------- */

bool j2534_filter_match(const uint8_t *mask, size_t mask_len,
                        const uint8_t *pattern, size_t pattern_len,
                        const uint8_t *msg, size_t msg_len)
{
    if (mask_len == 0)
    {
        return true; /* PASS-all */
    }

    if (mask == NULL || pattern == NULL || msg == NULL)
    {
        return false;
    }

    size_t n = mask_len < msg_len ? mask_len : msg_len;

    for (size_t i = 0; i < n; i++)
    {
        uint8_t pat = (i < pattern_len) ? pattern[i] : 0;

        if (((msg[i] ^ pat) & mask[i]) != 0)
        {
            return false;
        }
    }

    return true;
}

/* ---- names ------------------------------------------------------------ */

const char *j2534_status_name(uint32_t status)
{
    switch (status)
    {
        case J2534_STATUS_NOERROR:           return "NOERROR";
        case J2534_ERR_NOT_SUPPORTED:        return "ERR_NOT_SUPPORTED";
        case J2534_ERR_INVALID_CHANNEL_ID:   return "ERR_INVALID_CHANNEL_ID";
        case J2534_ERR_INVALID_PROTOCOL_ID:  return "ERR_INVALID_PROTOCOL_ID";
        case J2534_ERR_NULL_PARAMETER:       return "ERR_NULL_PARAMETER";
        case J2534_ERR_INVALID_FLAGS:        return "ERR_INVALID_FLAGS";
        case J2534_ERR_FAILED:               return "ERR_FAILED";
        case J2534_ERR_DEVICE_NOT_CONNECTED: return "ERR_DEVICE_NOT_CONNECTED";
        case J2534_ERR_TIMEOUT:              return "ERR_TIMEOUT";
        case J2534_ERR_INVALID_MSG:          return "ERR_INVALID_MSG";
        case J2534_ERR_BUFFER_EMPTY:         return "ERR_BUFFER_EMPTY";
        case J2534_ERR_BUFFER_FULL:          return "ERR_BUFFER_FULL";
        case J2534_ERR_BUFFER_OVERFLOW:      return "ERR_BUFFER_OVERFLOW";
        case J2534_ERR_INVALID_FILTER_ID:    return "ERR_INVALID_FILTER_ID";
        case J2534_ERR_NO_FLOW_CONTROL:      return "ERR_NO_FLOW_CONTROL";
        case J2534_ERR_INVALID_BAUDRATE:     return "ERR_INVALID_BAUDRATE";
        case J2534_ERR_INVALID_DEVICE_ID:    return "ERR_INVALID_DEVICE_ID";
        case J2534_ERR_DEVICE_IN_USE:        return "ERR_DEVICE_IN_USE";
        default:                             return "?";
    }
}

/* ---- transport-agnostic framer ---------------------------------------- */

/* read exactly n bytes into buf (or discard them when buf == NULL) */
static j2534_frame_rc_t read_exact(j2534_read_fn rd, void *ctx,
                                   uint32_t per_read_ms,
                                   uint32_t max_idle_reads,
                                   volatile const bool *run, uint8_t *buf,
                                   size_t n)
{
    uint8_t scratch[32];
    size_t got = 0;
    uint32_t idle = 0;

    while (got < n)
    {
        if (run != NULL && !*run)
        {
            return J2534_FR_STOPPED;
        }

        uint8_t *dst = (buf != NULL) ? buf + got : scratch;
        size_t want = n - got;

        if (buf == NULL && want > sizeof(scratch))
        {
            want = sizeof(scratch);
        }

        int r = rd(ctx, dst, want, per_read_ms);

        if (r < 0)
        {
            return J2534_FR_LINK_DOWN;
        }

        if (r == 0)
        {
            if (max_idle_reads != 0 && ++idle >= max_idle_reads)
            {
                return J2534_FR_TIMEOUT;
            }

            continue;
        }

        idle = 0;
        got += (size_t)r;
    }

    return J2534_FR_OK;
}

j2534_frame_rc_t j2534_frame_read(j2534_read_fn rd, void *ctx,
                                  uint32_t per_read_ms,
                                  uint32_t max_idle_reads,
                                  volatile const bool *run, j2534_hdr_t *h,
                                  uint8_t *payload, size_t payload_cap)
{
    uint8_t hdr[J2534_HDR_SIZE];
    j2534_frame_rc_t rc = read_exact(rd, ctx, per_read_ms, max_idle_reads,
                                     run, hdr, sizeof(hdr));

    if (rc != J2534_FR_OK)
    {
        return rc;
    }

    if (!j2534_hdr_decode(hdr, sizeof(hdr), h))
    {
        return J2534_FR_BAD_HDR;
    }

    if (h->length > payload_cap)
    {
        return J2534_FR_TOO_BIG;
    }

    if (h->length == 0)
    {
        return J2534_FR_OK;
    }

    return read_exact(rd, ctx, per_read_ms, max_idle_reads, run, payload,
                      h->length);
}

size_t j2534_ack_encode(uint8_t *buf, size_t cap, uint16_t seq,
                        uint16_t channel, uint32_t status,
                        const uint32_t *result)
{
    uint32_t len = (result != NULL) ? 8 : 4;

    if (buf == NULL || cap < J2534_HDR_SIZE + len)
    {
        return 0;
    }

    j2534_hdr_encode(buf, J2534_MT_ACK, seq, channel, len);
    put_u32(buf + J2534_HDR_SIZE, status);

    if (result != NULL)
    {
        put_u32(buf + J2534_HDR_SIZE + 4, *result);
    }

    return J2534_HDR_SIZE + len;
}

bool j2534_tcp_gate_allowed(bool allow_lan, bool loopback, bool on_ap,
                            bool on_usb)
{
    return allow_lan || loopback || on_ap || on_usb;
}
