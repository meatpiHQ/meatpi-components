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
 * @file button_manager.c
 * @brief Lifecycle + the polled button task. NO interrupts by design
 *        (meatpi 2026-07-19): a 1 s gpio_get_level poll — the legacy
 *        config_mode.c cadence — is plenty for a human hold and immune
 *        to ISR/glitch classes.
 */
#include "button_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "log_manager.h"

#include "button_manager_private.h"

static const char *TAG = "button_manager";

#define BTN_TICK_MS 1000 /* legacy CONFIG_MODE_TICK_MS */

static button_manager_longpress_cb_t s_cb;
static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;   /* internal: FreeRTOS object */
static StackType_t s_task_stack[4096]; /* INTERNAL stack: the callback
    reaches radio paths (wifi/BLE teardown) which the §2 corollary bars
    from PSRAM stacks */
static volatile bool s_running;

static void button_task(void *arg)
{
    btn_press_t press;

    (void)arg;
    btn_press_reset(&press);

    while (s_running)
    {
        bool pressed = (gpio_get_level(BUTTON_MANAGER_GPIO) == 0);

        if (btn_press_step(&press, pressed, btn_settings_hold_s()))
        {
            ESP_LOGI(TAG, "long-press (%lus) — firing callback",
                     (unsigned long)btn_settings_hold_s());

            if (s_cb != NULL)
            {
                s_cb();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_TICK_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t button_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "button_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC); /* per-TAG level control (§9.2) */
    return btn_settings_register();
}

esp_err_t button_manager_start(void)
{
    if (!btn_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (!btn_settings_enabled())
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
    }

    /* plain polled input, pull-up, active low — NO interrupt */
    gpio_reset_pin(BUTTON_MANAGER_GPIO);
    gpio_set_direction(BUTTON_MANAGER_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_MANAGER_GPIO, GPIO_PULLUP_ONLY);

    s_running = true;
    s_task = xTaskCreateStatic(button_task, "button_mgr",
                               sizeof(s_task_stack) /
                                   sizeof(s_task_stack[0]),
                               NULL, 5, s_task_stack, &s_task_tcb);

    ESP_LOGI(TAG, "started (gpio %d, poll %d ms, hold %lus)",
             BUTTON_MANAGER_GPIO, BTN_TICK_MS,
             (unsigned long)btn_settings_hold_s());
    return ESP_OK;
}

esp_err_t button_manager_stop(void)
{
    s_running = false;
    return ESP_OK;
}

esp_err_t button_manager_set_longpress_cb(button_manager_longpress_cb_t cb)
{
    s_cb = cb;
    return ESP_OK;
}
