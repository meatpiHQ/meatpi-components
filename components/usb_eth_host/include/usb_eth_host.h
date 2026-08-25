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
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum usb_eth_host_driver
{
    USB_ETH_HOST_DRIVER_RTL8152 = 0,
    USB_ETH_HOST_DRIVER_ASIX = 1,
    USB_ETH_HOST_DRIVER_CDC_ECM = 2,
    USB_ETH_HOST_DRIVER_CDC_NCM = 3,
    USB_ETH_HOST_DRIVER_RNDIS = 4,
    USB_ETH_HOST_DRIVER_MAX
} usb_eth_host_driver_t;

#define USB_ETH_HOST_DRIVER_MASK(d) (1UL << (uint32_t)(d))

#define USB_ETH_HOST_DRIVER_MASK_RTL8152 USB_ETH_HOST_DRIVER_MASK(USB_ETH_HOST_DRIVER_RTL8152)
#define USB_ETH_HOST_DRIVER_MASK_ASIX    USB_ETH_HOST_DRIVER_MASK(USB_ETH_HOST_DRIVER_ASIX)
#define USB_ETH_HOST_DRIVER_MASK_CDC_ECM USB_ETH_HOST_DRIVER_MASK(USB_ETH_HOST_DRIVER_CDC_ECM)
#define USB_ETH_HOST_DRIVER_MASK_CDC_NCM USB_ETH_HOST_DRIVER_MASK(USB_ETH_HOST_DRIVER_CDC_NCM)
#define USB_ETH_HOST_DRIVER_MASK_RNDIS   USB_ETH_HOST_DRIVER_MASK(USB_ETH_HOST_DRIVER_RNDIS)

#define USB_ETH_HOST_DRIVER_MASK_ALL (USB_ETH_HOST_DRIVER_MASK_RTL8152 | \
                                     USB_ETH_HOST_DRIVER_MASK_ASIX | \
                                     USB_ETH_HOST_DRIVER_MASK_CDC_ECM | \
                                     USB_ETH_HOST_DRIVER_MASK_CDC_NCM | \
                                     USB_ETH_HOST_DRIVER_MASK_RNDIS)

typedef struct usb_eth_host_gpio
{
    int vbus_en_gpio;
    bool vbus_en_active_high;
    uint32_t vbus_on_delay_ms;
} usb_eth_host_gpio_t;

typedef enum usb_eth_host_ip_mode
{
    USB_ETH_HOST_IP_MODE_DHCP = 0,
    USB_ETH_HOST_IP_MODE_STATIC = 1,
} usb_eth_host_ip_mode_t;

typedef struct usb_eth_host_netif_config
{
    usb_eth_host_ip_mode_t mode;
    esp_netif_ip_info_t static_ip;

    // If true, make this interface the default route (preferred internet uplink).
    // When enabled, usb_eth_host will also re-assert this after Wi-Fi gets an IP.
    bool prefer_as_default_route;
} usb_eth_host_netif_config_t;

typedef struct usb_eth_host_config
{
    bool enable;

    uint32_t allowed_driver_mask;

    int bus_id;
    uintptr_t reg_base;

    usb_eth_host_gpio_t gpio;

    usb_eth_host_netif_config_t netif;

    // Optional callbacks fired when the USB ETH interface gains or loses an IP address.
    // Called from the IP_EVENT handler (system event task context).
    void (*on_eth_ip_up)(void);
    void (*on_eth_ip_lost)(void);
} usb_eth_host_config_t;

esp_err_t usb_eth_host_start(const usb_eth_host_config_t *config);
void usb_eth_host_stop(void);
bool usb_eth_host_is_started(void);

bool usb_eth_host_driver_is_allowed(usb_eth_host_driver_t driver);

const char *usb_eth_host_driver_to_str(usb_eth_host_driver_t driver);

bool usb_eth_host_get_active_driver(usb_eth_host_driver_t *driver);
bool usb_eth_host_get_active_ifkey(char *ifkey, size_t ifkey_len);

// USB idVendor/idProduct of the device behind the active driver (e.g. the
// ESPNetLink dongle is 0x303A:0x4007). False when no driver is active.
bool usb_eth_host_get_active_device_ids(uint16_t *vid, uint16_t *pid);

// Reports the RNDIS media/carrier state as last seen by the class driver.
// Returns ESP_ERR_NOT_FOUND if no RNDIS device is attached.
esp_err_t usb_eth_host_rndis_get_link(bool *connected);

// Returns the netif config passed to usb_eth_host_start().
// Intended for internal component use by the USB-ETH drivers.
const usb_eth_host_netif_config_t *usb_eth_host_get_netif_config(void);

// Applies IP configuration to an already-created esp-netif (e.g., ifkey "u4").
// This is suitable for driving later from a UI / config server.
esp_err_t usb_eth_host_netif_apply(const char *ifkey, const usb_eth_host_netif_config_t *cfg);

#ifdef __cplusplus
}
#endif
