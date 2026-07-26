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
 * @file usb_host_manager_settings.c
 * @brief settings_manager descriptor for usb_host_manager: field-table
 *        schema (source of truth for shape/ranges/defaults) and on_apply
 *        (parses into the boot-applied role/class/IP config read by
 *        _start(); registers the CLI, settings-gated per standard §6b).
 */
#include <stdio.h>
#include <string.h>

#include "esp_netif.h"

#include "settings_manager.h"

#include "usb_host_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    /* When enabled, which USB stack the connector's ESP-OTG runs.
     * `host`   = CherryUSB host (USB-Ethernet adapters, espnetlink — today).
     * `device` = CherryUSB DEVICE — WiCAN presents itself to the PC; the
     *  class is `device_class`. Default `host` keeps existing enabled=true
     *  configs unchanged. `enabled=false` leaves the connector on the
     *  CH342 (serial console/flash) regardless. Host vs device is fixed
     *  at boot (the S3 USB-OTG is one or the other). Phase 3 —
     *  TASK_j2534_server.md §5. */
    SETTINGS_STR_ENUM("role", "host,device", "host"),
    /* device role only: which USB-device class WiCAN presents.
     * `ncm`/`rndis` = a USB-Ethernet DEVICE (WiCAN is a NIC to the PC) →
     *  IP-over-USB, which carries the J2534 TCP server + web UI + bridges
     *  over the one USB cable (usb_net_device). `cdc` = CDC-ACM virtual
     *  COM port carrying the J2534 wire protocol over serial
     *  (usb_cdc_device; own PID, WICAN_USB_DEV_PID_CDC). NCM is the
     *  modern/cross-platform default; RNDIS is the legacy Windows-native
     *  fallback; cdc is for tools that want a serial J2534 device. */
    SETTINGS_STR_ENUM("device_class", "ncm,rndis,cdc", "ncm"),
    SETTINGS_STR_ENUM("ip_mode", "dhcp,static", "dhcp"),
    SETTINGS_STR("static_ip", 15, ""),
    SETTINGS_STR("static_netmask", 15, "255.255.255.0"),
    SETTINGS_STR("static_gw", 15, ""),
    SETTINGS_BOOL("prefer_usb_route", false),
    SETTINGS_BOOL("cli", true),
};

/* boot-applied settings */
static bool s_enabled;
static bool s_role_device;   /* enabled && role=="device" (CherryUSB device, Phase 3) */
static char s_device_class[8];   /* "ncm" | "rndis" | "cdc" */
static bool s_prefer_usb_route;
static bool s_ip_static;
static esp_netif_ip_info_t s_static_ip;
static bool s_configured;

bool uhm_settings_enabled(void)
{
    return s_enabled;
}

bool uhm_settings_role_device(void)
{
    return s_role_device;
}

const char *uhm_settings_device_class(void)
{
    return s_device_class;
}

bool uhm_settings_prefer_usb_route(void)
{
    return s_prefer_usb_route;
}

bool uhm_settings_ip_static(void)
{
    return s_ip_static;
}

const esp_netif_ip_info_t *uhm_settings_static_ip(void)
{
    return &s_static_ip;
}

bool uhm_settings_is_configured(void)
{
    return s_configured;
}

static void parse_ip(const cJSON *settings, const char *key,
                     esp_ip4_addr_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings, key);

    out->addr = 0;

    if (cJSON_IsString(item) && item->valuestring[0] != '\0')
    {
        (void)esp_netif_str_to_ip4(item->valuestring, out);
    }
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item;

    bool feature_on = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "role");
    s_role_device = feature_on && cJSON_IsString(item) &&
                    strcmp(item->valuestring, "device") == 0;
    item = cJSON_GetObjectItemCaseSensitive(settings, "device_class");
    snprintf(s_device_class, sizeof(s_device_class), "%s",
             (cJSON_IsString(item) && item->valuestring[0]) ?
             item->valuestring : "ncm");
    /* host stack runs only in the host role; device role is the J2534
     * USB-CDC path (Phase 3) — it does NOT bring up CherryUSB host */
    s_enabled = feature_on && !s_role_device;
    item = cJSON_GetObjectItemCaseSensitive(settings, "ip_mode");
    s_ip_static = cJSON_IsString(item) &&
                  strcmp(item->valuestring, "static") == 0;
    parse_ip(settings, "static_ip", &s_static_ip.ip);
    parse_ip(settings, "static_netmask", &s_static_ip.netmask);
    parse_ip(settings, "static_gw", &s_static_ip.gw);
    s_prefer_usb_route = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "prefer_usb_route"));

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered &&
            usb_host_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    uhm_status_set_enabled(s_enabled);
    s_configured = true;
    return ESP_OK;
}

esp_err_t uhm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "usb_host_manager",
        .version = 1,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
    };

    return settings_manager_register(&DESC);
}
