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
 * @file interface_manager.c
 * @brief Lifecycle and the arbitration task (see the public header for
 *        the rules; interface_manager_policy.c is the pure rule table).
 *        Settings live in interface_manager_settings.c (standard §4.1).
 */
#include "interface_manager.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "dev_status_manager.h"
#include "log_manager.h"
#include "wifi_manager.h"

#include "interface_manager_private.h"

static const char *TAG = "interface_manager";

#define IM_POLL_MS     500
#define IM_DEBOUNCE    2   /* consecutive identical evaluations to act */

static bool s_started;

static TaskHandle_t s_task;
static StaticTask_t s_tcb; /* internal: FreeRTOS object */
/* INTERNAL stack: actuations can end in esp_wifi mode changes (possible
 * NVS writes) and BLE stack setup — §2 corollary territory */
static StackType_t s_stack[4096];

/* what the policy currently holds suspended (device-side truth) */
static im_target_t s_active;

/* ---- actuation -------------------------------------------------------------------- */

static void gather_inputs(im_inputs_t *in)
{
    const im_config_t *cfg = im_settings_config();
    EventBits_t bits = dev_status_manager_get();

    in->mode_has_sta = wifi_manager_mode_has_sta();
    in->mode_has_ap = wifi_manager_mode_has_ap();
    /* the CONFIGURED truth — stable while we hold the stack stopped
       (the BLE_ENABLED bit tracks the running state, not the config) */
    in->ble_enabled = ble_manager_is_enabled();
    in->ble_connected = (bits & DEV_STATUS_BIT_BLE_CONNECTED) != 0;
    in->ap_has_clients = wifi_manager_get_ap_station_count() > 0;
    in->rule_sta_ble_handover = cfg->sta_ble_handover;
    in->rule_ap_ble_exclusive = cfg->ap_ble_exclusive;
}

static void actuate(const im_target_t *want)
{
    if (want->suspend_sta != s_active.suspend_sta)
    {
        esp_err_t err = want->suspend_sta ? wifi_manager_suspend_sta()
                                          : wifi_manager_resume_sta();

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "STA %s (BLE %s)",
                     want->suspend_sta ? "suspended" : "resumed",
                     want->suspend_sta ? "client connected"
                                       : "client left");
            s_active.suspend_sta = want->suspend_sta;

            if (want->suspend_sta)
            {
                dev_status_manager_set(DEV_STATUS_BIT_STA_SUSPENDED);
            }
            else
            {
                dev_status_manager_clear(DEV_STATUS_BIT_STA_SUSPENDED);
            }
        }
    }

    if (want->suspend_ap != s_active.suspend_ap)
    {
        esp_err_t err = want->suspend_ap ? wifi_manager_suspend_ap()
                                         : wifi_manager_resume_ap();

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "AP %s", want->suspend_ap ? "suspended"
                                                    : "resumed");
            s_active.suspend_ap = want->suspend_ap;

            if (want->suspend_ap)
            {
                dev_status_manager_set(DEV_STATUS_BIT_AP_SUSPENDED);
            }
            else
            {
                dev_status_manager_clear(DEV_STATUS_BIT_AP_SUSPENDED);
            }
        }
    }

    if (want->suspend_ble != s_active.suspend_ble)
    {
        esp_err_t err = want->suspend_ble ? ble_manager_stop()
                                          : ble_manager_start();

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "BLE %s (AP %s)",
                     want->suspend_ble ? "stopped" : "restarted",
                     want->suspend_ble ? "station joined"
                                       : "station left");
            s_active.suspend_ble = want->suspend_ble;

            if (want->suspend_ble)
            {
                dev_status_manager_set(DEV_STATUS_BIT_BLE_SUSPENDED);
            }
            else
            {
                dev_status_manager_clear(DEV_STATUS_BIT_BLE_SUSPENDED);
            }
        }
    }
}

static void arbitration_task(void *arg)
{
    im_target_t pending = { 0 };
    int stable = 0;

    (void)arg;
    ESP_LOGD(TAG, "arbitration task up");

    while (s_started)
    {
        vTaskDelay(pdMS_TO_TICKS(IM_POLL_MS));

        im_inputs_t in;
        im_target_t want;

        gather_inputs(&in);
        im_policy_evaluate(&in, &want);

        /* debounce: act only after the SAME target held for N polls
           (rides out connect/disconnect flapping) */
        if (want.suspend_sta == pending.suspend_sta &&
            want.suspend_ap == pending.suspend_ap &&
            want.suspend_ble == pending.suspend_ble)
        {
            if (stable < IM_DEBOUNCE)
            {
                stable++;
            }
        }
        else
        {
            pending = want;
            stable = 1;
        }

        if (stable >= IM_DEBOUNCE)
        {
            actuate(&pending);
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle ---------------------------------------------------------------------- */

esp_err_t interface_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "interface_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    return im_settings_register();
}

esp_err_t interface_manager_start(void)
{
    const im_config_t *cfg = im_settings_config();

    if (!im_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (s_started)
    {
        return ESP_OK;
    }

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    s_started = true;
    s_task = xTaskCreateStatic(arbitration_task, "iface_arb",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               4, s_stack, &s_tcb);

    if (s_task == NULL)
    {
        s_started = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started (sta_ble_handover=%d, ap_ble_exclusive=%d)",
             cfg->sta_ble_handover, cfg->ap_ble_exclusive);
    return ESP_OK;
}

esp_err_t interface_manager_stop(void)
{
    s_started = false;

    for (int i = 0; i < 25 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* never leave interfaces stuck suspended without their arbiter */
    static const im_target_t NOTHING = { 0 };

    actuate(&NOTHING);
    return ESP_OK;
}
