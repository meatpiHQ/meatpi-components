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
 * @file usb_host_manager_events.c
 * @brief event_manager glue: `usb.device {attached}` and
 *        `usb.eth {connected, ip}` sources. Publishes come from the
 *        presence task / esp_event task (queue-safe).
 */
#include <stdio.h>

#include "event_manager.h"

#include "usb_host_manager_private.h"

void uhm_events_register(void)
{
    static const em_key_decl_t DEVICE_KEYS[] =
    {
        { "attached", EM_VAL_STR },
    };
    static const em_source_decl_t DEVICE =
    {
        .source = "usb", .name = "device",
        .description = "a USB device was attached to / detached from "
                       "the host port (debounced ID pin)",
        .keys = DEVICE_KEYS, .n_keys = 1,
    };
    static const em_key_decl_t ETH_KEYS[] =
    {
        { "connected", EM_VAL_STR },
        { "ip", EM_VAL_STR },
    };
    static const em_source_decl_t ETH =
    {
        .source = "usb", .name = "eth",
        .description = "the USB-Ethernet uplink gained/lost an IP "
                       "address",
        .keys = ETH_KEYS, .n_keys = 2,
    };

    (void)event_manager_declare_source(&DEVICE);
    (void)event_manager_declare_source(&ETH);
}

void uhm_events_device(bool attached)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "usb");
    snprintf(ev.name, sizeof(ev.name), "device");
    ev.kv[0] = em_kv_str("attached", attached ? "true" : "false");
    ev.n = 1;
    (void)event_manager_publish(&ev);
}

void uhm_events_eth(bool connected, const char *ip)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "usb");
    snprintf(ev.name, sizeof(ev.name), "eth");
    ev.kv[0] = em_kv_str("connected", connected ? "true" : "false");
    ev.kv[1] = em_kv_str("ip", ip != NULL ? ip : "");
    ev.n = 2;
    (void)event_manager_publish(&ev);
}
