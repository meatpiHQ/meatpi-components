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
 * @file mqtt_can_codec.c
 * @brief PURE codec (no I/O, no FreeRTOS): legacy-JSON batch encoder,
 *        fragmented-stream object reassembler, tx parser. Host-tested.
 *
 * Encode is hand-rolled snprintf (deterministic, heap-free — this runs
 * per batch at CAN rates); the low-rate tx parse uses cJSON.
 */
#include "mqtt_can_codec.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "can_frame_wire.h"

/* ---- rx: batcher ----------------------------------------------------------- */

void mc_batch_init(mc_batch_t *b, uint16_t batch_frames)
{
    memset(b, 0, sizeof(*b));

    if (batch_frames < 1)
    {
        batch_frames = 1;
    }

    if (batch_frames > MC_BATCH_MAX)
    {
        batch_frames = MC_BATCH_MAX;
    }

    b->limit = batch_frames;
}

int mc_batch_add_chunk(mc_batch_t *b, const uint8_t *chunk, size_t len)
{
    size_t off = 0;
    int added = 0;

    while (off + CAN_WIRE_HDR <= len)
    {
        can_core_frame_t f;
        size_t used = can_wire_decode_next(chunk + off, len - off, &f);

        if (used == 0)
        {
            break;              /* malformed tail — stop, keep what fits */
        }

        off += used;

        if (b->n >= MC_BATCH_MAX)
        {
            b->dropped++;
            continue;
        }

        b->frames[b->n++] = f;
        added++;
    }

    return added;
}

bool mc_batch_ready(const mc_batch_t *b)
{
    return b->n >= b->limit;
}

size_t mc_batch_json(mc_batch_t *b, int64_t ts_ms, char *out, size_t cap)
{
    size_t w = 0;

    if (b->n == 0)
    {
        return 0;
    }

    int n = snprintf(out + w, cap - w,
                     "{\"bus\":\"0\",\"type\":\"rx\",\"ts\":%lld,"
                     "\"frame\":[", (long long)ts_ms);

    if (n < 0 || (size_t)n >= cap - w)
    {
        b->n = 0;
        return 0;
    }

    w += (size_t)n;

    for (uint16_t i = 0; i < b->n; i++)
    {
        const can_core_frame_t *f = &b->frames[i];
        uint8_t dlc = (f->dlc > 8) ? 8 : f->dlc;

        n = snprintf(out + w, cap - w,
                     "%s{\"id\":%lu,\"dlc\":%u,\"rtr\":%s,\"extd\":%s,"
                     "\"data\":[",
                     (i > 0) ? "," : "", (unsigned long)f->id, dlc,
                     f->rtr ? "true" : "false",
                     f->ext ? "true" : "false");

        if (n < 0 || (size_t)n >= cap - w)
        {
            b->n = 0;
            return 0;
        }

        w += (size_t)n;

        for (uint8_t d = 0; d < dlc; d++)
        {
            n = snprintf(out + w, cap - w, "%s%u",
                         (d > 0) ? "," : "", f->data[d]);

            if (n < 0 || (size_t)n >= cap - w)
            {
                b->n = 0;
                return 0;
            }

            w += (size_t)n;
        }

        n = snprintf(out + w, cap - w, "]}");

        if (n < 0 || (size_t)n >= cap - w)
        {
            b->n = 0;
            return 0;
        }

        w += (size_t)n;
    }

    n = snprintf(out + w, cap - w, "]}");

    if (n < 0 || (size_t)n >= cap - w)
    {
        b->n = 0;
        return 0;
    }

    w += (size_t)n;
    b->n = 0;
    return w;
}

/* ---- tx: stream reassembler ------------------------------------------------ */

void mc_txasm_init(mc_txasm_t *a)
{
    memset(a, 0, sizeof(*a));
}

int mc_txasm_feed(mc_txasm_t *a, const uint8_t *in, size_t len,
                  mc_obj_cb_t cb, void *arg)
{
    int done = 0;

    for (size_t i = 0; i < len; i++)
    {
        char ch = (char)in[i];

        if (a->depth == 0)
        {
            /* between objects: ignore everything until an opener */
            if (ch != '{')
            {
                continue;
            }

            a->len = 0;
            a->overflow = false;
        }

        if (a->len < MC_TXBUF_MAX)
        {
            a->buf[a->len++] = ch;
        }
        else
        {
            a->overflow = true;
        }

        if (a->in_str)
        {
            if (a->esc)
            {
                a->esc = false;
            }
            else if (ch == '\\')
            {
                a->esc = true;
            }
            else if (ch == '"')
            {
                a->in_str = false;
            }

            continue;
        }

        if (ch == '"')
        {
            a->in_str = true;
        }
        else if (ch == '{')
        {
            a->depth++;
        }
        else if (ch == '}')
        {
            a->depth--;

            if (a->depth <= 0)
            {
                a->depth = 0;

                if (a->overflow)
                {
                    a->oversize++;
                }
                else if (cb != NULL)
                {
                    cb(arg, a->buf, a->len);
                    done++;
                }

                a->len = 0;
                a->overflow = false;
            }
        }
    }

    return done;
}

/* ---- tx: parser ------------------------------------------------------------ */

int mc_tx_parse(const char *json, size_t len, can_core_frame_t *out,
                size_t max)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    int count = -1;

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "frame");

    if (cJSON_IsString(type) && strcmp(type->valuestring, "tx") == 0 &&
        cJSON_IsArray(arr))
    {
        const cJSON *fr = NULL;

        count = 0;
        cJSON_ArrayForEach(fr, arr)
        {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(fr, "id");
            const cJSON *dlc = cJSON_GetObjectItemCaseSensitive(fr, "dlc");
            const cJSON *rtr = cJSON_GetObjectItemCaseSensitive(fr, "rtr");
            const cJSON *ext = cJSON_GetObjectItemCaseSensitive(fr,
                                                                "extd");
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(fr,
                                                                 "data");

            if (!cJSON_IsNumber(id) || id->valuedouble < 0 ||
                id->valuedouble > 0x1FFFFFFF ||
                !cJSON_IsNumber(dlc) || dlc->valueint < 0 ||
                dlc->valueint > 8)
            {
                count = -1;
                break;
            }

            if ((size_t)count >= max)
            {
                break;          /* keep what fits                        */
            }

            can_core_frame_t *f = &out[count];

            memset(f, 0, sizeof(*f));
            f->id = (uint32_t)id->valuedouble;
            f->dlc = (uint8_t)dlc->valueint;
            f->rtr = cJSON_IsTrue(rtr);
            f->ext = cJSON_IsTrue(ext) || f->id > 0x7FF;

            for (uint8_t d = 0; d < f->dlc; d++)
            {
                const cJSON *byte = cJSON_IsArray(data)
                    ? cJSON_GetArrayItem(data, d) : NULL;

                if (cJSON_IsNumber(byte))
                {
                    f->data[d] = (uint8_t)byte->valueint;
                }
            }

            count++;
        }
    }

    cJSON_Delete(root);
    return count;
}
