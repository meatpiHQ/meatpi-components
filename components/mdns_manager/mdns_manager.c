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
 * @file mdns_manager.c
 * @brief Lifecycle + the esp-mdns glue (see include/mdns_manager.h for
 *        the preserved Home-Assistant discovery contract). Settings live
 *        in mdns_manager_settings.c (standard §4.1).
 */
#include "mdns_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

#include "dev_status_manager.h"
#include "log_manager.h"

#include "mdns_manager_private.h"

static const char *TAG = "mdns_manager";

/* the legacy on-air strings — the HA integration matches these */
#define MM_INSTANCE     "wican web server"
#define MM_SERVICE_NAME "WiCAN-WebServer"
#define MM_SERVICE_TYPE "_wican"
#define MM_SERVICE_PROT "_tcp"
#define MM_SERVICE_PORT 80

/* device-contract v2 (ha_webhooks/device-contract): every MeatPi
 * product advertises _meatpi._tcp — the one brand-wide discovery
 * surface. _wican._tcp stays in parallel for older integrations. */
#define MM_MEATPI_SERVICE_TYPE "_meatpi"
#define MM_API_LEVEL_STR       "6"

static char s_hostname[24];
static char s_hostname_local[36];
static bool s_started;

esp_err_t mdns_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "mdns_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return mdns_manager_settings_register();
}

esp_err_t mdns_manager_start(void)
{
    if (!mdns_manager_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    const char *id = dev_status_manager_device_id();

    mm_build_hostname(id, s_hostname, sizeof(s_hostname));
    mm_build_hostname_local(id, s_hostname_local,
                            sizeof(s_hostname_local));

    if (!mdns_manager_settings_enabled())
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    esp_err_t err = mdns_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(MM_INSTANCE);

    /* the TXT contract: legacy keys, real values (legacy shipped empty
     * firmware/hardware/version strings on the Pro — same keys) */
    uint8_t mac[6];
    char mac_str[18];

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mm_format_mac(mac, mac_str, sizeof(mac_str));

    const char *fw = dev_status_manager_app_version();
    mdns_txt_item_t txt[] =
    {
        { "mac", mac_str },              /* HA's stable unique ID       */
        { "device_id", (char *)id },
        { "device_type", CONFIG_WICAN_DEVICE_TYPE }, /* contract v2     */
        { "firmware", (char *)fw },
        { "hardware", CONFIG_WICAN_HW_VERSION },
        { "version", (char *)fw },
        { "path", "/" },
    };

    err = mdns_service_add(MM_SERVICE_NAME, MM_SERVICE_TYPE,
                           MM_SERVICE_PROT, MM_SERVICE_PORT, txt,
                           sizeof(txt) / sizeof(txt[0]));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "service add failed: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }

    /* the contract-v2 brand service (integration 3.0 discovers ONLY
       _meatpi/_wican service types — _http matching was removed) */
    mdns_txt_item_t meatpi_txt[] =
    {
        { "device_type", CONFIG_WICAN_DEVICE_TYPE },
        { "device_id", (char *)id },
        { "mac", mac_str },
        { "fw", (char *)fw },
        { "api", MM_API_LEVEL_STR },
    };

    err = mdns_service_add(MM_SERVICE_NAME, MM_MEATPI_SERVICE_TYPE,
                           MM_SERVICE_PROT, MM_SERVICE_PORT, meatpi_txt,
                           sizeof(meatpi_txt) / sizeof(meatpi_txt[0]));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "meatpi service add failed: %s",
                 esp_err_to_name(err));
        mdns_service_remove_all();
        mdns_free();
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "advertising %s (%s.%s + %s.%s port %d) as %s",
             s_hostname_local, MM_SERVICE_TYPE, MM_SERVICE_PROT,
             MM_MEATPI_SERVICE_TYPE, MM_SERVICE_PROT,
             MM_SERVICE_PORT, MM_SERVICE_NAME);
    return ESP_OK;
}

esp_err_t mdns_manager_stop(void)
{
    if (s_started)
    {
        mdns_service_remove_all();
        mdns_free();
        s_started = false;
    }

    return ESP_OK;
}

const char *mdns_manager_hostname(void)
{
    return s_hostname_local;
}
