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
 * @file ble_manager_channel.c
 * @brief The stream-channel registry (stack-agnostic): pre-start
 *        registration, per-channel RX StreamBuffer fed by the GATT write
 *        callback, blocking TX as indications or notifications (the mode
 *        the central selected with its CCCD, among what the owner allows),
 *        link-generation bookkeeping so a reader sees exactly one <0 per
 *        dropped link and never reads stale bytes.
 */
#include "ble_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

#define BLM_CH_READ_SLICE_MS 100 /* link-down detection granularity */

typedef struct
{
    ble_manager_channel_desc_t  desc;
    StreamBufferHandle_t        sb;
    StaticStreamBuffer_t        sb_obj;      /* internal: FreeRTOS object */
    uint16_t                    in_handle;   /* filled by the GATT layer  */
    uint16_t                    out_handle;  /* filled by the GATT layer  */
    volatile bool               sub_notify;  /* CCCD bits (host task)     */
    volatile bool               sub_indicate;
    volatile uint32_t           link_gen;    /* bumps on every disconnect */
    uint32_t                    reader_gen;  /* last gen the reader saw   */
    ble_manager_channel_stats_t stats;
} blm_channel_t;

/* small (~4 x 180 B) and holds FreeRTOS objects: internal .bss */
static blm_channel_t s_ch[BLE_MANAGER_CHANNEL_MAX];
static int s_count;
static bool s_locked;

/* ---- registration --------------------------------------------------------------- */

static bool uuid_taken(uint16_t uuid)
{
    if (uuid == 0xFFF0 || uuid == 0xFFF1 || uuid == 0xFFF2)
    {
        return true;
    }

    for (int i = 0; i < s_count; i++)
    {
        if (s_ch[i].desc.uuid_in == uuid || s_ch[i].desc.uuid_out == uuid)
        {
            return true;
        }
    }

    return false;
}

esp_err_t ble_manager_channel_register(const ble_manager_channel_desc_t *desc,
                                       int *out_id)
{
    if (desc == NULL || desc->name == NULL || desc->rx_storage == NULL ||
        desc->rx_size < BLE_MANAGER_CHANNEL_RX_MIN ||
        desc->uuid_in == desc->uuid_out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_locked)
    {
        ESP_LOGW(TAG, "channel '%s' refused: registry frozen (post-start)",
                 desc->name);
        return ESP_ERR_INVALID_STATE;
    }

    if (uuid_taken(desc->uuid_in) || uuid_taken(desc->uuid_out))
    {
        ESP_LOGW(TAG, "channel '%s' refused: UUID in use", desc->name);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_count >= BLE_MANAGER_CHANNEL_MAX)
    {
        /* bounded registry overflow is load-bearing (standard §12) */
        ESP_LOGE(TAG, "channel registry full (%d): '%s' not registered",
                 BLE_MANAGER_CHANNEL_MAX, desc->name);
        return ESP_ERR_NO_MEM;
    }

    blm_channel_t *c = &s_ch[s_count];

    memset(c, 0, sizeof(*c));
    c->desc = *desc;
    /* trigger level 1: a reader wakes on the first byte */
    c->sb = xStreamBufferCreateStatic(desc->rx_size, 1, desc->rx_storage,
                                      &c->sb_obj);

    if (c->sb == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (out_id != NULL)
    {
        *out_id = s_count;
    }

    ESP_LOGI(TAG, "channel %d '%s' registered (out %04X in %04X, rx %u B)",
             s_count, desc->name, desc->uuid_out, desc->uuid_in,
             (unsigned)desc->rx_size);
    s_count++;
    return ESP_OK;
}

void blm_channel_lock(void)
{
    s_locked = true;
}

int blm_channel_count(void)
{
    return s_count;
}

const ble_manager_channel_desc_t *blm_channel_desc(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_ch[idx].desc : NULL;
}

void blm_channel_set_in_handle(int idx, uint16_t attr_handle)
{
    if (idx >= 0 && idx < s_count)
    {
        s_ch[idx].in_handle = attr_handle;
    }
}

void blm_channel_set_out_handle(int idx, uint16_t attr_handle)
{
    if (idx >= 0 && idx < s_count)
    {
        s_ch[idx].out_handle = attr_handle;
    }
}

uint8_t blm_channel_out_modes(int idx)
{
    if (idx < 0 || idx >= s_count)
    {
        return 0;
    }

    uint8_t m = s_ch[idx].desc.out_modes;

    return (m == 0) ? BLE_MANAGER_CH_OUT_INDICATE : m;
}

int blm_channel_find_in_handle(uint16_t attr_handle)
{
    for (int i = 0; i < s_count; i++)
    {
        if (s_ch[i].in_handle == attr_handle && attr_handle != 0)
        {
            return i;
        }
    }

    return -1;
}

void ble_manager_channel_capacity(size_t *used, size_t *cap)
{
    if (used != NULL)
    {
        *used = (size_t)s_count;
    }

    if (cap != NULL)
    {
        *cap = BLE_MANAGER_CHANNEL_MAX;
    }
}

/* ---- host-task side -------------------------------------------------------------- */

void blm_channel_on_subscribe(uint16_t attr_handle, bool notify, bool indicate)
{
    for (int i = 0; i < s_count; i++)
    {
        blm_channel_t *c = &s_ch[i];

        if (c->out_handle != attr_handle || attr_handle == 0)
        {
            continue;
        }

        c->sub_notify = notify;
        c->sub_indicate = indicate;
        ESP_LOGI(TAG, "channel '%s' out mode: %s (cccd notify=%d indicate=%d)",
                 c->desc.name,
                 ble_manager_channel_out_name(ble_manager_channel_out_mode(i)),
                 notify, indicate);
        return;
    }
}

bool blm_channel_on_rx(int idx, const uint8_t *data, size_t len)
{
    if (idx < 0 || idx >= s_count)
    {
        return false;
    }

    blm_channel_t *c = &s_ch[idx];
    size_t sent = xStreamBufferSend(c->sb, data, len, 0);

    c->stats.rx_bytes += (uint32_t)sent;

    if (sent != len)
    {
        c->stats.rx_overflow++;

        if ((c->stats.rx_overflow % 50) == 1)
        {
            ESP_LOGW(TAG, "channel '%s' rx overflow (%lu)", c->desc.name,
                     (unsigned long)c->stats.rx_overflow);
        }

        return false;
    }

    return true;
}

void blm_channel_on_link(ble_manager_channel_event_t evt)
{
    for (int i = 0; i < s_count; i++)
    {
        blm_channel_t *c = &s_ch[i];

        if (evt == BLE_MANAGER_CH_DISCONNECTED)
        {
            c->link_gen++; /* the reader discards leftovers on its next call */
            /* a bonded central's CCCDs come back as SUBSCRIBE events
               (reason RESTORE) on its next connection */
            c->sub_notify = false;
            c->sub_indicate = false;
        }

        if (c->desc.on_event != NULL)
        {
            c->desc.on_event(i, evt, c->desc.arg);
        }
    }
}

/* ---- reader / writer side --------------------------------------------------------- */

int ble_manager_channel_read(int id, uint8_t *buf, size_t n,
                             uint32_t timeout_ms)
{
    if (id < 0 || id >= s_count || buf == NULL || n == 0)
    {
        return -1;
    }

    blm_channel_t *c = &s_ch[id];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        if (c->reader_gen != c->link_gen)
        {
            /* the link dropped since the last read: drop what the old
               peer left behind (reader context: reset is legal here) */
            c->reader_gen = c->link_gen;
            xStreamBufferReset(c->sb);
            return -1;
        }

        if (!blm_gatt_connected())
        {
            return -1;
        }

        int32_t remaining = (int32_t)(deadline - xTaskGetTickCount());
        TickType_t slice = pdMS_TO_TICKS(BLM_CH_READ_SLICE_MS);

        if (remaining <= 0)
        {
            slice = 0;
        }
        else if ((TickType_t)remaining < slice)
        {
            slice = (TickType_t)remaining;
        }

        size_t got = xStreamBufferReceive(c->sb, buf, n, slice);

        if (got > 0)
        {
            return (int)got;
        }

        if (remaining <= 0)
        {
            return 0;
        }
    }
}

int ble_manager_channel_write(int id, const uint8_t *buf, size_t n)
{
    if (id < 0 || id >= s_count || (buf == NULL && n > 0))
    {
        return -1;
    }

    blm_channel_t *c = &s_ch[id];
    uint16_t handle = blm_gatt_channel_out_handle(id);

    if (handle == 0 || !blm_gatt_connected() || !blm_gatt_secured())
    {
        c->stats.tx_link_down++;
        return -1;
    }

    size_t off = 0;
    bool notify = (ble_manager_channel_out_mode(id) == BLE_MANAGER_CH_OUT_NOTIFY);

    while (off < n)
    {
        size_t max_data = blm_gatt_max_data();
        size_t chunk = n - off;

        if (chunk > max_data)
        {
            chunk = max_data;
        }

        /* indications: each PDU confirmed by the central before the next;
           notifications: queued, bounded ENOMEM retry (the owner's
           protocol carries credits + a frame counter, see the header) */
        esp_err_t err = notify
            ? blm_gatt_notify_handle(handle, buf + off, (uint16_t)chunk,
                                     BLM_CHANNEL_TX_WAIT_MS)
            : blm_gatt_indicate_handle(handle, buf + off, (uint16_t)chunk,
                                       BLM_CHANNEL_TX_WAIT_MS);

        if (err == ESP_ERR_TIMEOUT)
        {
            c->stats.tx_timeouts++;
            ESP_LOGW(TAG, "channel '%s' tx credit timeout at %u/%u",
                     c->desc.name, (unsigned)off, (unsigned)n);
            return -2;
        }

        if (err != ESP_OK)
        {
            c->stats.tx_link_down++;

            if ((c->stats.tx_link_down % 20) == 1)
            {
                ESP_LOGW(TAG, "channel '%s' tx failed (%s) at %u/%u",
                         c->desc.name, esp_err_to_name(err), (unsigned)off,
                         (unsigned)n);
            }

            return -1;
        }

        off += chunk;
        c->stats.tx_bytes += (uint32_t)chunk;

        if (notify)
        {
            c->stats.tx_notifications++;
        }
        else
        {
            c->stats.tx_indications++;
        }
    }

    return (int)n;
}

uint8_t ble_manager_channel_out_mode(int id)
{
    if (id < 0 || id >= s_count)
    {
        return BLE_MANAGER_CH_OUT_NONE;
    }

    blm_channel_t *c = &s_ch[id];

    return blm_channel_pick_out(c->desc.out_modes, c->sub_notify,
                                c->sub_indicate);
}

const char *ble_manager_channel_out_name(uint8_t mode)
{
    switch (mode)
    {
        case BLE_MANAGER_CH_OUT_INDICATE: return "indicate";
        case BLE_MANAGER_CH_OUT_NOTIFY:   return "notify";
        default:                          return "none";
    }
}

uint16_t ble_manager_channel_pdu_max(void)
{
    return blm_gatt_max_data();
}

esp_err_t ble_manager_channel_stats(int id, ble_manager_channel_stats_t *out)
{
    if (id < 0 || id >= s_count || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_ch[id].stats;
    out->rx_pending = xStreamBufferBytesAvailable(s_ch[id].sb);
    out->out_mode = (s_ch[id].sub_notify || s_ch[id].sub_indicate)
                        ? ble_manager_channel_out_mode(id)
                        : BLE_MANAGER_CH_OUT_NONE;
    return ESP_OK;
}
