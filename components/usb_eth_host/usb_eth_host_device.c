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
 * @file usb_eth_host_device.c
 * @brief The attached USB device, whatever its class (2026-10-07): CherryUSB's
 *        mount / unmount hooks (MeatPi additions to the vendored core) record
 *        idVendor, idProduct, the device and first-interface classes and the
 *        product string, so the status surfaces can name a GNSS receiver
 *        bound as CDC-ACM or a memory stick nobody binds. Before this only
 *        the device behind an Ethernet driver had ids (`vid`/`pid`).
 */
#include "usb_eth_host.h"

#include <string.h>

#include "esp_log.h"

#include "usbh_core.h"

static const char *TAG = "usb_eth_host";

static usb_eth_host_device_t g_dev;
static volatile bool g_dev_present;

void usbh_device_mount_done_callback(struct usbh_hubport *hport,
                                     const char *product)
{
    usb_eth_host_device_t d;

    memset(&d, 0, sizeof(d));
    if (hport == NULL)
    {
        return;
    }
    d.vid = hport->device_desc.idVendor;
    d.pid = hport->device_desc.idProduct;
    d.dev_class = hport->device_desc.bDeviceClass;
    d.interfaces = hport->config.config_desc.bNumInterfaces;
    if (d.interfaces > 0)
    {
        d.intf_class = hport->config.intf[0].altsetting[0].intf_desc.bInterfaceClass;
    }
    if (product != NULL)
    {
        strncpy(d.product, product, sizeof(d.product) - 1);
    }

    g_dev = d;
    g_dev_present = true;
    ESP_LOGI(TAG, "USB device %04x:%04x (class %02x, interface class %02x, "
             "%u interfaces) '%s'", d.vid, d.pid, d.dev_class, d.intf_class,
             d.interfaces, d.product);
}

void usbh_device_unmount_done_callback(struct usbh_hubport *hport)
{
    (void)hport;
    g_dev_present = false;
}

bool usb_eth_host_get_attached_device(usb_eth_host_device_t *out)
{
    if (!g_dev_present)
    {
        return false;
    }
    if (out != NULL)
    {
        *out = g_dev;
    }
    return true;
}
