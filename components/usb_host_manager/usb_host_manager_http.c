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
 * @file usb_host_manager_http.c
 * @brief The optional /api/usb status route (§9.1).
 */
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "usb_host_manager.h"

static const char *TAG = "usb_host_manager";

/* the USB class code as a word, for the pages (the wizard names a device
 * by its ids and product string; this is what the class means) */
static const char *class_name(uint8_t c)
{
    switch (c)
    {
        case 0x02: return "cdc";
        case 0x0a: return "cdc_data";
        case 0x03: return "hid";
        case 0x08: return "mass_storage";
        case 0x09: return "hub";
        case 0xe0: return "wireless";
        case 0xff: return "vendor";
        default:   return "other";
    }
}

/* this port does not read USB string descriptors at enumeration
 * (CONFIG_USBHOST_GET_STRING_DESC off: a failed string read would fail
 * the whole enumeration in CherryUSB), so the product name of the devices
 * WiCAN knows comes from here; an unknown device shows its ids */
static const char *known_product(uint16_t vid, uint16_t pid)
{
    if (vid == 0x303a && pid == 0x4007) return "ESPNetLink";
    if (vid == 0x1546) return "u-blox GNSS receiver";
    if (vid == 0x0b95) return "ASIX USB Ethernet adapter";
    if (vid == 0x0bda && (pid == 0x8152 || pid == 0x8153)) return "Realtek USB Ethernet adapter";
    return "";
}

static esp_err_t usb_handler(httpd_req_t *req)
{
    usb_host_manager_status_t st;
    char body[440];
    char product[sizeof(st.dev_product)];

    (void)usb_host_manager_status(&st);
    if (st.dev_present && st.dev_product[0] == '\0')
    {
        snprintf(st.dev_product, sizeof(st.dev_product), "%s",
                 known_product(st.dev_vid, st.dev_pid));
    }

    /* the product string is the device's own: keep it JSON-safe */
    for (size_t i = 0; i < sizeof(product); i++)
    {
        char c = st.dev_product[i];

        product[i] = (c == '"' || c == '\\' || (c != '\0' && c < 0x20)) ? ' ' : c;
        if (c == '\0')
        {
            break;
        }
    }
    product[sizeof(product) - 1] = '\0';

    int n = snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"device_present\":%s,\"host_active\":%s,"
             "\"eth_connected\":%s,\"driver\":\"%s\",\"ip\":\"%s\","
             "\"attaches\":%lu,\"vid\":\"%04x\",\"pid\":\"%04x\","
             "\"vbus\":%s,",
             st.enabled ? "true" : "false",
             st.device_present ? "true" : "false",
             st.host_active ? "true" : "false",
             st.eth_connected ? "true" : "false",
             st.driver, st.ip, (unsigned long)st.attaches,
             st.vid, st.pid, st.vbus_on ? "true" : "false");

    /* 2026-10-07: the enumerated device, whatever its class */
    if (st.dev_present && n > 0 && (size_t)n < sizeof(body))
    {
        snprintf(body + n, sizeof(body) - (size_t)n,
                 "\"device\":{\"vid\":\"%04x\",\"pid\":\"%04x\","
                 "\"class\":\"%s\",\"product\":\"%s\"}}",
                 st.dev_vid, st.dev_pid, class_name(st.dev_class), product);
    }
    else if (n > 0 && (size_t)n < sizeof(body))
    {
        snprintf(body + n, sizeof(body) - (size_t)n, "\"device\":null}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t usb_host_manager_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/usb", .method = HTTP_GET,
          .handler = usb_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/usb registered");
    }

    return err;
}
