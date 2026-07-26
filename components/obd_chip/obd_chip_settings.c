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
 * @file obd_chip_settings.c
 * @brief Settings descriptor ("obd_chip", field table, reboot-to-apply §4.2):
 *        chip UART baud, auto-sleep, and the monitor arbitration policy
 *        (reserved; v1 implements "manual" only — task §5 open question).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "settings_manager.h"

#include "obd_chip_private.h"

/* clang-format off */
static const settings_field_t OBD_FIELDS[] =
{
    SETTINGS_STR_ENUM("baud", "115200,230400,460800,921600,2000000", "2000000"),
    SETTINGS_BOOL    ("auto_sleep",     false),
    /* legacy parity (Ali 2026-07-26): a chip not at the packaged fw
       version is auto-flashed to it after bring-up */
    SETTINGS_BOOL    ("auto_update",    true),
    SETTINGS_STR_ENUM("monitor_policy", "manual,auto_interrupt", "manual"),
    /* chip sleep-config provisioning (defaults = the legacy fallbacks;
       the chip's autonomous controls stay OFF — the future sleep_manager
       arms them) */
    SETTINGS_INT     ("wake_voltage_mv",  8000, 16000, 13500),
    SETTINGS_INT     ("sleep_voltage_mv", 8000, 16000, 13200),
    SETTINGS_INT     ("sleep_time_min",   1, 1440, 2),
};
/* clang-format on */

static obd_config_t s_config EXT_RAM_BSS_ATTR;
static bool s_configured;

static esp_err_t obd_on_apply(const cJSON *settings)
{
    const cJSON *baud = cJSON_GetObjectItemCaseSensitive(settings, "baud");
    const cJSON *auto_sleep =
        cJSON_GetObjectItemCaseSensitive(settings, "auto_sleep");
    const cJSON *policy =
        cJSON_GetObjectItemCaseSensitive(settings, "monitor_policy");

    const cJSON *wake_mv =
        cJSON_GetObjectItemCaseSensitive(settings, "wake_voltage_mv");
    const cJSON *sleep_mv =
        cJSON_GetObjectItemCaseSensitive(settings, "sleep_voltage_mv");
    const cJSON *sleep_min =
        cJSON_GetObjectItemCaseSensitive(settings, "sleep_time_min");

    s_config.baud = (cJSON_IsString(baud) && baud->valuestring != NULL)
                        ? atoi(baud->valuestring) : 2000000;
    s_config.auto_sleep = cJSON_IsTrue(auto_sleep);
    s_config.auto_update = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "auto_update"));
    s_config.monitor_auto_interrupt =
        cJSON_IsString(policy) && policy->valuestring != NULL &&
        strcmp(policy->valuestring, "auto_interrupt") == 0;
    s_config.wake_voltage =
        (cJSON_IsNumber(wake_mv) ? (float)wake_mv->valueint : 13500.0f) /
        1000.0f;
    s_config.sleep_voltage =
        (cJSON_IsNumber(sleep_mv) ? (float)sleep_mv->valueint : 13200.0f) /
        1000.0f;
    /* legacy: minutes*60 + 30 s guard (the ESP sleeps first) */
    s_config.sleep_time_s =
        (uint32_t)(cJSON_IsNumber(sleep_min) ? sleep_min->valueint : 2) *
            60 + 30;

    s_configured = true;
    return ESP_OK;
}

esp_err_t obd_settings_register(void)
{
    static const settings_descriptor_t desc =
    {
        .name        = "obd_chip",
        .version     = 1,
        .fields      = OBD_FIELDS,
        .field_count = sizeof(OBD_FIELDS) / sizeof(OBD_FIELDS[0]),
        .on_apply    = obd_on_apply,
    };

    return settings_manager_register(&desc);
}

const obd_config_t *obd_settings_config(void)
{
    return &s_config;
}

bool obd_settings_is_configured(void)
{
    return s_configured;
}
