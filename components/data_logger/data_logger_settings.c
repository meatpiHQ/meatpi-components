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
 * @file data_logger_settings.c
 * @brief settings_manager descriptor for data_logger: field-table schema
 *        (source of truth for shape/ranges/defaults), on_apply (parses
 *        into the boot-applied config, snapshots the CAN subscription
 *        config, then re-binds the stream engines via dl_core_apply —
 *        data_logger.c owns the stream table and the rings).
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "settings_manager.h"

#include "data_logger.h" /* data_logger_register_cli (settings-gated) */
#include "data_logger_private.h"

static const char *TAG = "data_logger";

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_STR_ENUM("format", "sqlite,csv,binary,jsonl", "sqlite"),
    SETTINGS_INT("max_file_mb", 1, 32, 4),
    SETTINGS_INT("max_files", 1, 500, 100),
    SETTINGS_INT("batch_rows", 16, 1024, 256),
    /* 5 s since 2026-09-07 (ROBUSTNESS.md "Fewer writes"): batches sit in
       PSRAM, commits are atomic, a crash/restart loses nothing — a power
       cut loses at most this much */
    SETTINGS_INT("flush_ms", 100, 60000, 5000),
    /* v2: producer integration + the CAN stream (TASK addendum);
     * mf4/blf/candump/asc/jsonl added by addendum 2 (enum additions
     * are schema-compatible — stored values stay valid) */
    SETTINGS_STR_ENUM("autopid_log", "off,changed,all", "off"),
    SETTINGS_BOOL("can_log", false),
    SETTINGS_STR_ENUM("can_format",
                      "binary,csv,sqlite,mf4,blf,candump,asc,jsonl",
                      "binary"),
    SETTINGS_STR("can_filter", 10, ""),  /* hex id; "" = ALL frames   */
    SETTINGS_STR("can_mask", 10, "7FF"), /* hex; used with can_filter */
    SETTINGS_BOOL("can_ext", false),
    SETTINGS_INT("can_max_file_mb", 1, 128, 8),
    SETTINGS_INT("can_max_files", 1, 500, 50),
    SETTINGS_INT("ring_len", 512, DL_CAN_RING_MAX, 2048),
    SETTINGS_BOOL("cli", true),
};

static dl_cfg_t s_cfg;
static bool s_configured;

const dl_cfg_t *dl_settings_config(void)
{
    return &s_cfg;
}

bool dl_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_migrate(uint32_t from_version, cJSON *settings)
{
    /* v1 -> v2: only new fields (schema defaults fill them) */
    (void)from_version;
    (void)settings;
    return ESP_OK;
}

static uint32_t field_u32(const cJSON *settings, const char *key,
                          uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings, key);

    return cJSON_IsNumber(item) ? (uint32_t)item->valueint : fallback;
}

static const char *field_str(const cJSON *settings, const char *key,
                             const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings, key);

    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_cfg.enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));

    snprintf(s_cfg.format, sizeof(s_cfg.format), "%s",
             field_str(settings, "format", "sqlite"));
    snprintf(s_cfg.can_format, sizeof(s_cfg.can_format), "%s",
             field_str(settings, "can_format", "binary"));

    s_cfg.max_file_mb = field_u32(settings, "max_file_mb", 4);
    s_cfg.max_files = field_u32(settings, "max_files", 100);
    s_cfg.can_max_file_mb = field_u32(settings, "can_max_file_mb", 8);
    s_cfg.can_max_files = field_u32(settings, "can_max_files", 50);
    s_cfg.batch_rows = field_u32(settings, "batch_rows", 256);
    s_cfg.flush_ms = field_u32(settings, "flush_ms", 1000);
    s_cfg.ring_len = field_u32(settings, "ring_len", 2048);

    const char *ap = field_str(settings, "autopid_log", "off");

    s_cfg.autopid_log = (strcmp(ap, "all") == 0) ? 2
                        : (strcmp(ap, "changed") == 0) ? 1 : 0;

    /* CAN subscription config (data_logger_can.c applies it at start) */
    const char *filter = field_str(settings, "can_filter", "");

    s_cfg.can.log = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "can_log"));
    s_cfg.can.ext = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "can_ext"));
    s_cfg.can.monitor_all = (filter[0] == '\0');
    s_cfg.can.filter = 0;
    s_cfg.can.mask = 0x7FF;

    if (!s_cfg.can.monitor_all &&
        !dl_parse_hex_u32(filter, &s_cfg.can.filter))
    {
        ESP_LOGW(TAG, "bad can_filter '%s' — logging ALL frames",
                 filter);
        s_cfg.can.monitor_all = true;
    }

    (void)dl_parse_hex_u32(field_str(settings, "can_mask", "7FF"),
                           &s_cfg.can.mask);
    dl_can_configure(&s_cfg.can);

    /* engine binding + stream/ring knobs (writer-owned state) */
    dl_core_apply(&s_cfg);

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && data_logger_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t dl_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "data_logger",
        .version = 2,
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_migrate = on_migrate,
    };

    return settings_manager_register(&DESC);
}
