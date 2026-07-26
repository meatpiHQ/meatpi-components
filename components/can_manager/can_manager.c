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
 * @file can_manager.c
 * @brief Lifecycle + the shared can_core bus handle. Settings live in
 *        can_manager_settings.c (standard §4.1).
 */
#include "can_manager.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "log_manager.h"

#include "can_manager_private.h"

static const char *TAG = "can_manager";

/* ---- state ---------------------------------------------------------------- */

static bool s_started;
static bool s_running;

/* ONE handle per physical CAN peripheral (can_core contract) */
EXT_RAM_BSS_ATTR static can_core_handle_t s_bus;

/* ---- transceiver standby -------------------------------------------------- */

static void cm_standby(bool standby)
{
    gpio_set_direction(CONFIG_WICAN_CAN_STDBY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_WICAN_CAN_STDBY_GPIO, standby ? 1 : 0);
}

/* ---- public API ------------------------------------------------------------ */

esp_err_t can_manager_init(void)
{
    static const log_descriptor_t LOG_DESC = { "can_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return canm_settings_register();
}

esp_err_t can_manager_start(void)
{
    if (!canm_settings_is_configured())
    {
        return ESP_ERR_INVALID_STATE; /* §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    if (!canm_settings_enabled())
    {
        cm_standby(true); /* park the transceiver while unused */
        ESP_LOGI(TAG, "disabled by settings");
        return ESP_OK;
    }

    cm_standby(false);

    uint32_t baud_kbps = canm_settings_baud_kbps();
    bool silent = canm_settings_silent();

    can_core_config_t cfg =
    {
        .tx_gpio        = CONFIG_WICAN_CAN_TX_GPIO,
        .rx_gpio        = CONFIG_WICAN_CAN_RX_GPIO,
        .baud_kbps      = baud_kbps,
        .silent_mode    = silent,
        .rx_queue_depth = 32,
        .tx_queue_depth = 64,
    };

    if (can_core_init(&s_bus, &cfg) != ELM327_OK)
    {
        ESP_LOGE(TAG, "bus init failed (baud %lu)",
                 (unsigned long)baud_kbps);
        cm_standby(true);
        return ESP_FAIL; /* main logs + degrades (§3 no panics) */
    }

    s_running = true;
    ESP_LOGI(TAG, "up: %lu kbit/s tx=%d rx=%d stdby=%d%s",
             (unsigned long)baud_kbps, CONFIG_WICAN_CAN_TX_GPIO,
             CONFIG_WICAN_CAN_RX_GPIO, CONFIG_WICAN_CAN_STDBY_GPIO,
             silent ? " SILENT" : "");
    return ESP_OK;
}

esp_err_t can_manager_stop(void)
{
    if (s_running)
    {
        can_core_deinit(&s_bus);
        s_running = false;
    }

    cm_standby(true);
    s_started = false;
    return ESP_OK;
}

struct can_core_handle_s *can_manager_core_handle(void)
{
    return s_running ? &s_bus : NULL;
}

esp_err_t can_manager_send(uint32_t id, bool ext, bool rtr,
                           const uint8_t *data, uint8_t dlc)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (dlc > 8 || (dlc > 0 && data == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    can_core_frame_t f =
    {
        .id  = id,
        .ext = ext,
        .rtr = rtr,
        .dlc = dlc,
    };

    if (dlc > 0)
    {
        memcpy(f.data, data, dlc);
    }

    return (can_core_transmit(&s_bus, &f, 100) == ELM327_OK) ? ESP_OK
                                                               : ESP_FAIL;
}

esp_err_t can_manager_subscribe_queue(QueueHandle_t q, uint32_t filter,
                                      uint32_t mask, bool ext,
                                      bool monitor_all, int *out_idx)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (q == NULL || out_idx == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    can_core_queue_subscriber_t sub =
    {
        .queue       = q,
        .filter      = filter,
        .mask        = mask,
        .ext         = ext,
        .monitor_all = monitor_all,
        .active      = true,
    };

    int idx = can_core_register_rx_queue(&s_bus, &sub);

    if (idx < 0)
    {
        return ESP_ERR_NO_MEM;
    }

    *out_idx = idx;
    return ESP_OK;
}

esp_err_t can_manager_unsubscribe_queue(int idx)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    can_core_unregister_rx_queue(&s_bus, idx);
    return ESP_OK;
}

esp_err_t can_manager_status(can_manager_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = canm_settings_enabled();
    out->running = s_running;
    out->silent = canm_settings_silent();
    out->baud_kbps = canm_settings_baud_kbps();

    if (s_running)
    {
        can_core_get_stats(&s_bus, &out->stats);
    }

    return ESP_OK;
}

void can_manager_zero_stats(void)
{
    if (s_running)
    {
        can_core_reset_stats(&s_bus);
    }
}
