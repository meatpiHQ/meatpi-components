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
 * @file can_core_clients.c
 * @brief Who receives the frames of the shared CAN bus: the callback
 *        clients and the task-owned queue subscribers of a
 *        can_core_handle_t (register, unregister, filter, name, count).
 *        The RX task in can_core.c walks both tables once per frame.
 */

#include <string.h>

#include "can_core.h"

/* -------------------------------------------------------------------------
 * can_core_register_client
 * ------------------------------------------------------------------------- */
int can_core_register_client(can_core_handle_t *handle,
                                const can_core_client_t *client)
{
    if (!handle || !client)
    {
        return ELM327_ERR_INVALID_ARG;
    }

    for (int i = 0; i < CAN_CORE_MAX_CLIENTS; i++)
    {
        if (!handle->clients[i].active)
        {
            handle->clients[i]        = *client;
            handle->clients[i].active = true;
            return i;
        }
    }

    return ELM327_ERR_NO_MEM;
}

int can_core_register_rx_queue(can_core_handle_t *handle,
                                 const can_core_queue_subscriber_t *subscriber)
{
    if (!handle || !subscriber || subscriber->queue == NULL)
    {
        return ELM327_ERR_INVALID_ARG;
    }

    for (int i = 0; i < CAN_CORE_MAX_QUEUE_SUBSCRIBERS; i++)
    {
        if (!handle->queue_subscribers[i].active)
        {
            handle->queue_subscribers[i] = *subscriber;
            handle->queue_subscribers[i].drops = 0;
            handle->queue_subscribers[i].active = true;
            return i;
        }
    }

    return ELM327_ERR_NO_MEM;
}

/* -------------------------------------------------------------------------
 * can_core_unregister_client
 * ------------------------------------------------------------------------- */
void can_core_unregister_client(can_core_handle_t *handle, int idx)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_CLIENTS)
    {
        return;
    }
    memset(&handle->clients[idx], 0, sizeof(handle->clients[idx]));
}

void can_core_unregister_rx_queue(can_core_handle_t *handle, int idx)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS)
    {
        return;
    }

    memset(&handle->queue_subscribers[idx], 0,
           sizeof(handle->queue_subscribers[idx]));
}

void can_core_set_rx_queue_name(can_core_handle_t *handle, int idx,
                                const char *name)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS ||
        !handle->queue_subscribers[idx].active)
    {
        return;
    }

    handle->queue_subscribers[idx].name = name;
}

int can_core_rx_queue_count(const can_core_handle_t *handle)
{
    int n = 0;

    if (!handle)
    {
        return 0;
    }

    for (int i = 0; i < CAN_CORE_MAX_QUEUE_SUBSCRIBERS; i++)
    {
        if (handle->queue_subscribers[i].active)
        {
            n++;
        }
    }

    return n;
}

bool can_core_get_rx_queue(const can_core_handle_t *handle, int idx,
                           can_core_queue_subscriber_t *out)
{
    if (!handle || !out || idx < 0 ||
        idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS ||
        !handle->queue_subscribers[idx].active)
    {
        return false;
    }

    *out = handle->queue_subscribers[idx];
    return true;
}

/* -------------------------------------------------------------------------
 * can_core_set_filter
 * ------------------------------------------------------------------------- */
void can_core_set_filter(can_core_handle_t *handle,
                            int idx,
                            uint32_t filter,
                            uint32_t mask,
                            bool ext)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_CLIENTS)
    {
        return;
    }
    handle->clients[idx].filter = filter;
    handle->clients[idx].mask   = mask;
    handle->clients[idx].ext    = ext;
}

void can_core_set_rx_queue_filter(can_core_handle_t *handle,
                                    int idx,
                                    uint32_t filter,
                                    uint32_t mask,
                                    bool ext)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS)
    {
        return;
    }

    handle->queue_subscribers[idx].filter = filter;
    handle->queue_subscribers[idx].mask = mask;
    handle->queue_subscribers[idx].ext = ext;
}

/* -------------------------------------------------------------------------
 * can_core_set_monitor_all
 * ------------------------------------------------------------------------- */
void can_core_set_monitor_all(can_core_handle_t *handle,
                                 int idx,
                                 bool monitor_all)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_CLIENTS)
    {
        return;
    }
    handle->clients[idx].monitor_all = monitor_all;
}

void can_core_set_rx_queue_monitor_all(can_core_handle_t *handle,
                                         int idx,
                                         bool monitor_all)
{
    if (!handle || idx < 0 || idx >= CAN_CORE_MAX_QUEUE_SUBSCRIBERS)
    {
        return;
    }

    handle->queue_subscribers[idx].monitor_all = monitor_all;
}
