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
 * @file ble_manager.c
 * @brief Lifecycle, subscriber/CLI registries, status. Settings live in
 *        ble_manager_settings.c (blm_core_config / blm_settings_*).
 */
#include "ble_manager.h"

#include <string.h>

#include "esp_log.h"

#include "dev_status_manager.h"
#include "log_manager.h"

#include "ble_manager_private.h"

static const char *TAG = "ble_manager";

static bool s_started;

static QueueHandle_t s_sub;                 /* the ONE subscriber (bridge) */
static uint32_t s_sub_drops;
static ble_cli_handler_t s_cli_handler;
static volatile bool s_pairing_allowed = true;

/* ---- lifecycle ----------------------------------------------------------------- */

esp_err_t ble_manager_init(void)
{
    static const log_descriptor_t LOG_DESC = { "ble_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return blm_settings_register();
}

esp_err_t ble_manager_start(void)
{
    const blm_config_t *cfg = blm_core_config();

    if (!blm_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled; BLE stack stays down");
        s_started = true;
        return ESP_OK;
    }

    s_pairing_allowed = cfg->pairing_at_boot;

    /* the on-air identity: "WiC_<device id>" (legacy ble_uid) */
    static char name[BLM_NAME_MAX]; /* GATT layer keeps a pointer */

    blm_ident_name(dev_status_manager_device_id(), name, sizeof(name));

    esp_err_t err = blm_gatt_stack_up(name);

    if (err != ESP_OK)
    {
        return err;
    }

    err = blm_io_start();

    if (err != ESP_OK)
    {
        blm_gatt_stack_down();
        return err;
    }

    s_started = true;
    dev_status_manager_set(DEV_STATUS_BIT_BLE_ENABLED);
    ESP_LOGI(TAG, "started as '%s' (tx %d dBm, pairing %s)", name,
             cfg->tx_power_dbm, s_pairing_allowed ? "on" : "off");
    return ESP_OK;
}

esp_err_t ble_manager_stop(void)
{
    if (!s_started)
    {
        return ESP_OK;
    }

    if (blm_core_config()->enabled)
    {
        blm_io_stop();
        blm_gatt_stack_down();
    }

    s_started = false;
    dev_status_manager_clear(DEV_STATUS_BIT_BLE_ENABLED |
                             DEV_STATUS_BIT_BLE_CONNECTED);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

/* ---- endpoint trio --------------------------------------------------------------- */

esp_err_t ble_manager_subscribe(QueueHandle_t q)
{
    if (q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sub != NULL)
    {
        return ESP_ERR_INVALID_STATE; /* one subscriber: one bridge */
    }

    s_sub = q;
    return ESP_OK;
}

esp_err_t ble_manager_unsubscribe(QueueHandle_t q)
{
    if (s_sub != q)
    {
        return ESP_ERR_NOT_FOUND;
    }

    s_sub = NULL;
    return ESP_OK;
}

esp_err_t ble_manager_send(const uint8_t *data, size_t len)
{
    if (!blm_gatt_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    return blm_io_queue_tx(data, len);
}

/* ---- status ------------------------------------------------------------------------ */

bool ble_manager_is_enabled(void)
{
    /* the CONFIGURED truth — stays true while interface_manager holds
       the stack stopped (the BLE_ENABLED bit tracks the RUNNING state,
       which is what a runtime stop clears) */
    return blm_settings_is_configured() && blm_core_config()->enabled;
}

bool ble_manager_is_connected(void)
{
    return blm_gatt_connected();
}

bool ble_manager_is_secured(void)
{
    return blm_gatt_secured();
}

/* ---- pairing window ----------------------------------------------------------------- */

void ble_manager_pairing_enable(void)
{
    s_pairing_allowed = true;
    blm_gatt_allow_pairing(true);
    ESP_LOGI(TAG, "pairing ENABLED");
}

void ble_manager_pairing_disable(void)
{
    s_pairing_allowed = false;
    blm_gatt_allow_pairing(false);
    ESP_LOGI(TAG, "pairing DISABLED");
}

bool ble_manager_pairing_is_enabled(void)
{
    return s_pairing_allowed;
}

bool blm_core_pairing_allowed(void)
{
    return s_pairing_allowed;
}

/* ---- CLI ------------------------------------------------------------------------------ */

esp_err_t ble_manager_set_cli_handler(ble_cli_handler_t handler)
{
    s_cli_handler = handler;
    return ESP_OK;
}

esp_err_t ble_manager_cli_write(const char *data, size_t len)
{
    return blm_io_cli_write(data, len);
}

/* ---- bridges from the GATT layer -------------------------------------------------------- */

void blm_core_on_rx(const uint8_t *data, size_t len)
{
    if (s_sub == NULL)
    {
        return; /* nobody attached: bytes fall on the floor by design */
    }

    ble_chunk_t chunk;

    while (len > 0)
    {
        size_t n = (len > BLE_MANAGER_CHUNK_SIZE) ? BLE_MANAGER_CHUNK_SIZE
                                                  : len;

        chunk.len = (uint16_t)n;
        memcpy(chunk.data, data, n);

        if (xQueueSend(s_sub, &chunk, 0) != pdTRUE)
        {
            s_sub_drops++; /* never block the BT callback */

            if ((s_sub_drops % 100) == 1)
            {
                ESP_LOGW(TAG, "subscriber queue full (%lu drops)",
                         (unsigned long)s_sub_drops);
            }
        }

        data += n;
        len -= n;
    }
}

void blm_core_on_cli_line(const char *line)
{
    if (s_cli_handler != NULL)
    {
        s_cli_handler(line);
    }
    else
    {
        ESP_LOGD(TAG, "CLI line dropped (no cmdline_manager yet): %s", line);
    }
}

void blm_core_on_connect(void)
{
    dev_status_manager_set(DEV_STATUS_BIT_BLE_CONNECTED);
    ESP_LOGI(TAG, "client connected");
}

void blm_core_on_disconnect(void)
{
    dev_status_manager_clear(DEV_STATUS_BIT_BLE_CONNECTED);
    ESP_LOGI(TAG, "client disconnected");
}
