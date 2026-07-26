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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "esp_event.h"
#include "esp_netif.h"

#include "usb_eth_host.h"

typedef struct usb_eth_netif_glue
{
    esp_netif_driver_base_t base;
    esp_event_handler_instance_t ins_got_ip;
    bool got_ip_registered;
    esp_err_t (*transmit)(void *h, void *buffer, size_t len);
    bool started;
} usb_eth_netif_glue_t;

esp_err_t usb_eth_netif_glue_start(usb_eth_netif_glue_t *glue,
                                  const char *if_key,
                                  const char *if_desc,
                                  const uint8_t mac[6],
                                  const usb_eth_host_netif_config_t *netif_cfg,
                                  esp_err_t (*transmit)(void *h, void *buffer, size_t len));

void usb_eth_netif_glue_stop(usb_eth_netif_glue_t *glue);
void usb_eth_netif_input(usb_eth_netif_glue_t *glue, uint8_t *buf, uint32_t len);
