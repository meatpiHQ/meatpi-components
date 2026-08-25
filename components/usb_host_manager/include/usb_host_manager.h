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
 * @file usb_host_manager.h
 * @brief WiCAN USB host policy (feature component) — rewrite of the
 *        legacy usb_host_manager (TASK_usb_host_manager.md), v1 scope
 *        = USB-Ethernet uplink.
 *
 * Model: the WiCAN Pro USB connector is shared between the CH342
 * (device role: PC console/flash) and the ESP32-S3 OTG host, muxed by
 * GPIO11. When `enabled`, a presence task watches the OTG ID pin
 * (GPIO39, low = a device/OTG cable attached, debounced): on attach
 * it flips the mux to host, powers VBUS (GPIO10, settle delay) and
 * starts usb_eth_host (CherryUSB enumerates the adapter — RTL8152 /
 * ASIX / CDC-ECM / CDC-NCM / RNDIS — into ONE esp_netif, DHCP or
 * static). On detach everything tears down and the mux returns to the
 * CH342, so a PC can always reach the console/flash through the port
 * while nothing else is plugged in — and ALWAYS when `enabled` is
 * false.
 *
 * Uplink semantics (v1, meatpi 2026-07-07): NO wifi/usb arbitration —
 * the wired uplink just raises DEV_STATUS_BIT_ETH_CONNECTED (part of
 * the NETWORK mask every consumer already gates on). `prefer_usb_route`
 * (default OFF = wifi-first, legacy default) only sets the default
 * netif when the wire has an IP. Internal-RAM note: the CherryUSB
 * eth buffers are a heap-backed shared pool that exists only while an
 * adapter is attached (cherryusb/PROVENANCE.md).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool enabled;
    bool device_present;   /**< debounced ID-pin state              */
    bool host_active;      /**< mux on OTG + CherryUSB running      */
    bool eth_connected;    /**< netif has an IP                     */
    char driver[12];       /**< active class ("rtl8152", ...)       */
    char ip[16];           /**< current address ("" when none)      */
    uint32_t attaches;     /**< attach edges since boot             */
    uint16_t vid;          /**< idVendor of the enumerated device   */
    uint16_t pid;          /**< idProduct (0 when none / unknown)   */
    bool vbus_on;          /**< rail state as last driven by us     */
} usb_host_manager_status_t;

/** Register descriptors (settings/log/events). No hardware access. */
esp_err_t usb_host_manager_init(void);

/** Configure the pins and start the presence task when enabled. */
esp_err_t usb_host_manager_start(void);

/** Tear the host stack down and return the mux to the CH342 (also the
 *  sleep path — wired into main's prepare callback). */
esp_err_t usb_host_manager_stop(void);

esp_err_t usb_host_manager_status(usb_host_manager_status_t *out);

/**
 * Drive the connector's VBUS rail (USB_OTG_PWR_EN, active high) while
 * host mode is active — the recovery lever for a dongle that must be
 * re-enumerated (espnetlink_link's key re-read), also behind the
 * `usb vbus <0|1>` dev command. The rail has a board pull-up, so the
 * power-on default is ON; an off→on cycle reboots the attached device.
 * @return ESP_ERR_INVALID_STATE when the host stack is not up.
 */
esp_err_t usb_host_manager_set_vbus(bool on);

/** Optional /api/usb route (§9.1; main wires it). */
esp_err_t usb_host_manager_register_http(void);

/** `usb` CLI; registered internally on the settings apply. */
esp_err_t usb_host_manager_register_cli(void);

#ifdef __cplusplus
}
#endif
