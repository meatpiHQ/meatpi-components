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

#include "sdkconfig.h"

/* RTL8152 support DISABLED (meatpi 2026-07-13): the r815x driver needs
 * a 32 KB internal-DMA RX pool (16 KB aggregation + accumulation
 * headroom — 16 KB exactly corrupts the heap, 2 KB refuses to
 * connect). Ruled not worth the RAM; ASIX is the supported adapter.
 * Re-enable by flipping CONFIG_CHERRYUSB_HOST_RTL8152 (Kconfig +
 * cherryusb/CMakeLists ESP branch) and sizing
 * CONFIG_USBHOST_RTL8152_ETH_MAX_RX_SIZE to 32 KB. */
#ifdef CONFIG_CHERRYUSB_HOST_RTL8152

#include "usb_eth_host_internal.h"

#include "esp_log.h"

#include "usb_osal.h"

#include "usbh_core.h"

#include "usb_eth_netif_glue_internal.h"

#include "usbh_rtl8152.h"

static const char *TAG = "usb_eth_rtl8152";

static usb_eth_netif_glue_t g_glue;

static esp_err_t usb_eth_rtl8152_transmit(void *h, void *buffer, size_t len)
{
    int ret;

    (void)h;

    usb_memcpy(usbh_rtl8152_get_eth_txbuf(), buffer, len);

    ret = usbh_rtl8152_eth_output((uint32_t)len);
    if (ret < 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void usbh_rtl8152_eth_input(uint8_t *buf, uint32_t buflen)
{
    usb_eth_netif_input(&g_glue, buf, buflen);
}

void usbh_rtl8152_run(struct usbh_rtl8152 *rtl8152_class)
{
    if (!usb_eth_host_is_started())
    {
        return;
    }

    if (!usb_eth_host_driver_is_allowed(USB_ETH_HOST_DRIVER_RTL8152))
    {
        ESP_LOGI(TAG, "RTL8152 blocked by runtime mask");
        return;
    }

    if (rtl8152_class == NULL)
    {
        return;
    }

    ESP_LOGI(TAG, "Starting RTL8152 netif");

    usb_eth_netif_glue_start(&g_glue,
                             "u4",
                             "usbh rtl8152",
                             rtl8152_class->mac,
                             usb_eth_host_get_netif_config(),
                             usb_eth_rtl8152_transmit);

    if (rtl8152_class->hport != NULL)
    {
        usb_eth_host_note_device_ids(rtl8152_class->hport->device_desc.idVendor,
                                     rtl8152_class->hport->device_desc.idProduct);
    }
    usb_eth_host_notify_driver_started(USB_ETH_HOST_DRIVER_RTL8152, "u4");

    usb_osal_thread_create("usbh_rtl8152_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rtl8152_rx_thread, NULL);
}

void usbh_rtl8152_stop(struct usbh_rtl8152 *rtl8152_class)
{
    (void)rtl8152_class;

    usb_eth_host_notify_driver_stopped(USB_ETH_HOST_DRIVER_RTL8152);
    usb_eth_netif_glue_stop(&g_glue);
}

#endif /* CONFIG_CHERRYUSB_HOST_RTL8152 */
