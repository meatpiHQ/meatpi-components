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
 * @file usb_net_device.h
 * @brief WiCAN as a USB-Ethernet DEVICE (Phase 3 of the J2534 plan,
 *        TASK_j2534_server.md §5).
 *
 * Runs the CherryUSB DEVICE stack on the S3's internal USB-OTG and presents
 * WiCAN to a PC as a USB network adapter. The link is bridged to an
 * esp_netif with a DHCP server, so plugging the cable in gives the PC an
 * IP-over-USB path to everything the device serves: the J2534 TCP server,
 * the web UI, and the TCP bridges.
 *
 * Class selection (usb_host_manager's `device_class` setting):
 *  - NCM   — CDC-NCM, the class Windows 10/11 binds natively (UsbNcm.sys).
 *            Windows 11 24H2 REMOVED the legacy RNDIS driver, so this is
 *            the default and the one to test first.
 *  - RNDIS — legacy fallback for pre-24H2 Windows.
 *
 * Ownership: usb_host_manager owns the role/mux decision and calls this in
 * `role=device`; this component owns the device stack, the netif, and the
 * DHCP server. Host and device stacks never run together (the OTG core is
 * one or the other, fixed at boot).
 *
 * RAM: the two NTB/frame buffers are USB-DMA and therefore INTERNAL heap
 * (~4 KB while active). Everything else (worker stack, RX copies) is PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    USB_NET_DEVICE_CLASS_NCM = 0,
    USB_NET_DEVICE_CLASS_RNDIS,
} usb_net_device_class_t;

typedef struct
{
    usb_net_device_class_t device_class;
    esp_ip4_addr_t ip;      /* WiCAN's address on the USB link; 0 = default
                             * 192.168.82.1 (the DHCP pool + gateway follow;
                             * NOT .80.x — that's the WiFi AP subnet) */
    esp_ip4_addr_t netmask; /* 0 = 255.255.255.0 */
} usb_net_device_config_t;

typedef struct
{
    bool configured;    /* host enumerated + configured the device */
    bool link_up;       /* data path open (NCM alt 1 / RNDIS filter set) */
    char ip[16];
    char device_class[8];
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_drops;
    uint32_t tx_drops;
} usb_net_device_status_t;

/** Bring up the device stack + netif. Caller has already muxed the
 *  connector to the ESP OTG. One-shot: stop/start cycling is not supported
 *  (role changes are reboot-to-apply anyway). */
esp_err_t usb_net_device_start(const usb_net_device_config_t *cfg);

esp_err_t usb_net_device_get_status(usb_net_device_status_t *out);

#ifdef __cplusplus
}
#endif
