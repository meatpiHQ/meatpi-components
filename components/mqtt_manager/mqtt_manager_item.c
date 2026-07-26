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
 * @file mqtt_manager_item.c
 * @brief PURE codec for the async publish queue's ring items — no RTOS,
 *        host-testable.
 */
#include <string.h>

#include "mqtt_manager_private.h"

size_t mm_item_size(size_t topic_strlen, size_t data_len)
{
    if (topic_strlen == 0 || topic_strlen + 1 > MM_ITEM_TOPIC_MAX ||
        data_len > MM_ITEM_DATA_MAX)
    {
        return 0;
    }

    return sizeof(mm_item_hdr_t) + topic_strlen + 1 + data_len;
}

bool mm_item_pack(uint8_t *buf, size_t buf_len, const char *topic,
                  const void *data, size_t data_len, int qos, bool retain)
{
    if (buf == NULL || topic == NULL || (data == NULL && data_len > 0) ||
        qos < 0 || qos > 2)
    {
        return false;
    }

    size_t topic_strlen = strlen(topic);
    size_t need = mm_item_size(topic_strlen, data_len);

    if (need == 0 || need > buf_len)
    {
        return false;
    }

    mm_item_hdr_t hdr =
    {
        .topic_len = (uint16_t)(topic_strlen + 1),
        .data_len = (uint16_t)data_len,
        .qos = (uint8_t)qos,
        .retain = retain ? 1 : 0,
    };

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), topic, topic_strlen + 1);

    if (data_len > 0)
    {
        memcpy(buf + sizeof(hdr) + topic_strlen + 1, data, data_len);
    }

    return true;
}

bool mm_item_unpack(const uint8_t *buf, size_t buf_len,
                    mm_item_hdr_t *hdr, const char **topic,
                    const uint8_t **data)
{
    if (buf == NULL || hdr == NULL || topic == NULL || data == NULL ||
        buf_len < sizeof(mm_item_hdr_t))
    {
        return false;
    }

    memcpy(hdr, buf, sizeof(*hdr));

    if (hdr->topic_len == 0 || hdr->topic_len > MM_ITEM_TOPIC_MAX ||
        hdr->data_len > MM_ITEM_DATA_MAX ||
        buf_len < sizeof(*hdr) + hdr->topic_len + hdr->data_len ||
        buf[sizeof(*hdr) + hdr->topic_len - 1] != '\0')
    {
        return false;
    }

    *topic = (const char *)(buf + sizeof(*hdr));
    *data = buf + sizeof(*hdr) + hdr->topic_len;
    return true;
}
