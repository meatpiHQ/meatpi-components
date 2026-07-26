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
 * @file log_sinks_settings.c
 * @brief settings_manager descriptor for log_sinks: field-table schema
 *        (every sink gate defaults FALSE — the device ships with no log
 *        byte leaving the box) and on_apply into the boot-applied config.
 */
#include <stdio.h>
#include <string.h>

#include "settings_manager.h"

#include "log_sinks.h"
#include "log_sinks_private.h"

static const settings_field_t FIELDS[] =
{
    /* live log-tail TCP server on the device (nc <ip> <port>) */
    SETTINGS_BOOL("tcp_enabled", false),
    SETTINGS_INT ("tcp_port", 1, 65535, 5515),
    /* push to a remote collector (one datagram per line batch) */
    SETTINGS_BOOL("udp_enabled", false),
    SETTINGS_STR ("udp_host", 63, ""),
    SETTINGS_INT ("udp_port", 1, 65535, 5514),
    /* text frames on the websocket_manager ws_log channel (/ws/log) */
    SETTINGS_BOOL("ws_enabled", false),
    /* rotated plain-text files on the SD card (/sd/devlog) */
    SETTINGS_BOOL("file_enabled", false),
    SETTINGS_INT ("file_max_kb", 64, 4096, 512),
    SETTINGS_INT ("file_keep", 1, LS_FILE_KEEP_MAX, 4),
    SETTINGS_INT ("file_flush_s", 5, 3600, 60),
    SETTINGS_BOOL("cli", true),
};

static log_sinks_config_t s_cfg =
{
    .tcp_port = 5515,
    .udp_port = 5514,
    .file_max_kb = 512,
    .file_keep = 4,
    .file_flush_s = 60,
};
static bool s_configured;

const log_sinks_config_t *ls_settings_config(void)
{
    return &s_cfg;
}

bool ls_settings_is_configured(void)
{
    return s_configured;
}

static int get_int(const cJSON *settings, const char *key, int min, int max,
                   int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    if (cJSON_IsNumber(v) && v->valueint >= min && v->valueint <= max)
    {
        return v->valueint;
    }

    return def;
}

static esp_err_t on_validate(const cJSON *settings, char *err,
                             size_t err_len)
{
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(settings,
                                                       "udp_enabled");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(settings,
                                                         "udp_host");

    if (cJSON_IsTrue(en) &&
        (!cJSON_IsString(host) || host->valuestring[0] == '\0'))
    {
        snprintf(err, err_len, "udp_host: required when udp_enabled");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(settings,
                                                         "udp_host");

    s_cfg.tcp_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "tcp_enabled"));
    s_cfg.tcp_port = (uint16_t)get_int(settings, "tcp_port", 1, 65535,
                                       5515);
    s_cfg.udp_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "udp_enabled"));
    s_cfg.udp_port = (uint16_t)get_int(settings, "udp_port", 1, 65535,
                                       5514);
    s_cfg.udp_host[0] = '\0';

    if (cJSON_IsString(host))
    {
        strlcpy(s_cfg.udp_host, host->valuestring, sizeof(s_cfg.udp_host));
    }

    s_cfg.ws_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "ws_enabled"));
    s_cfg.file_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "file_enabled"));
    s_cfg.file_max_kb = (uint16_t)get_int(settings, "file_max_kb", 64,
                                          4096, 512);
    s_cfg.file_keep = (uint8_t)get_int(settings, "file_keep", 1,
                                       LS_FILE_KEEP_MAX, 4);
    s_cfg.file_flush_s = (uint16_t)get_int(settings, "file_flush_s", 5,
                                           3600, 60);

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool reg;

        if (!reg && log_sinks_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t ls_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "log_sinks",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}
