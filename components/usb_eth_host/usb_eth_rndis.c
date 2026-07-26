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

#include "usb_eth_host_internal.h"

#include "esp_log.h"

#include "usb_osal.h"

#include "usbh_core.h"

#include "usb_eth_netif_glue_internal.h"

#include "usbh_rndis.h"

static const char *TAG = "usb_eth_rndis";

static usb_eth_netif_glue_t g_glue;

static esp_err_t usb_eth_rndis_transmit(void *h, void *buffer, size_t len)
{
    int ret;

    (void)h;

    usb_memcpy(usbh_rndis_get_eth_txbuf(), buffer, len);

    ret = usbh_rndis_eth_output((uint32_t)len);
    if (ret < 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void usbh_rndis_eth_input(uint8_t *buf, uint32_t buflen)
{
    usb_eth_netif_input(&g_glue, buf, buflen);
}

void usbh_rndis_run(struct usbh_rndis *rndis_class)
{
    if (!usb_eth_host_is_started())
    {
        return;
    }

    if (!usb_eth_host_driver_is_allowed(USB_ETH_HOST_DRIVER_RNDIS))
    {
        ESP_LOGI(TAG, "RNDIS blocked by runtime mask");
        return;
    }

    if (rndis_class == NULL)
    {
        return;
    }

    ESP_LOGI(TAG, "Starting RNDIS netif");

    usb_eth_netif_glue_start(&g_glue,
                             "u2",
                             "usbh rndis",
                             rndis_class->mac,
                             usb_eth_host_get_netif_config(),
                             usb_eth_rndis_transmit);

    usb_eth_host_notify_driver_started(USB_ETH_HOST_DRIVER_RNDIS, "u2");

    usb_osal_thread_create("usbh_rndis_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rndis_rx_thread, NULL);
}

void usbh_rndis_stop(struct usbh_rndis *rndis_class)
{
    (void)rndis_class;

    usb_eth_host_notify_driver_stopped(USB_ETH_HOST_DRIVER_RNDIS);
    usb_eth_netif_glue_stop(&g_glue);
}
