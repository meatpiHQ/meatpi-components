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
 * @file mqtt_manager_settings.c
 * @brief settings_manager descriptor for mqtt_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_validate (enabled
 *        needs a broker url), on_apply (stores the boot-applied broker
 *        config). The identity pair (client_id/topic_prefix) is seeded into
 *        mqtt_manager.c via mm_core_set_identity: start() resolves its
 *        device-id defaults in place.
 */
#include <stdio.h>
#include <string.h>

#include "settings_manager.h"

#include "mqtt_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_STR("url", 127, ""),             /* mqtt:// or mqtts://      */
    SETTINGS_STR("username", 63, ""),
    SETTINGS_STR("broker_password", 63, ""),  /* _password -> redacted    */
    SETTINGS_STR("client_id", 63, ""),        /* "" = wican_<device_id>   */
    SETTINGS_STR("topic_prefix", 63, ""),     /* "" = wican/<device_id>   */
    SETTINGS_STR("ca_file", 127, ""),         /* "" = built-in bundle     */
    SETTINGS_STR("cert_set", 24, ""),         /* cert_manager set; wins   */
    SETTINGS_INT("keepalive_s", 5, 600, 30),
    /* v2 (2026-10-01): the client follows the uplink instead of retrying
       into the void. "wifi" = run only while the WiFi station is connected
       (fresh default, Ali); "any" = any uplink incl. USB Ethernet (the v1
       behaviour, kept for migrated documents) */
    SETTINGS_STR_ENUM("connect_on", "wifi,any", "wifi"),
};

/* boot-applied settings (buffers outlive the client) */
static mm_config_t s_cfg;
static bool s_configured;

const mm_config_t *mm_settings_config(void)
{
    return &s_cfg;
}

bool mm_settings_is_configured(void)
{
    return s_configured;
}

static void copy_str(char *dst, size_t cap, const cJSON *obj,
                     const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    snprintf(dst, cap, "%s",
             cJSON_IsString(item) ? item->valuestring : "");
}

static esp_err_t on_validate(const cJSON *settings, char *err,
                             size_t err_len)
{
    const cJSON *enabled =
        cJSON_GetObjectItemCaseSensitive(settings, "enabled");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(settings, "url");

    if (cJSON_IsTrue(enabled) &&
        !(cJSON_IsString(url) && mm_url_valid(url->valuestring)))
    {
        snprintf(err, err_len,
                 "enabled needs url: mqtt://host[:port] or mqtts://...");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(settings, "enabled");
    char client_id[64];
    char prefix[64];

    s_cfg.enabled = cJSON_IsTrue(item);
    copy_str(s_cfg.url, sizeof(s_cfg.url), settings, "url");
    copy_str(s_cfg.username, sizeof(s_cfg.username), settings, "username");
    copy_str(s_cfg.password, sizeof(s_cfg.password), settings,
             "broker_password");
    copy_str(client_id, sizeof(client_id), settings, "client_id");
    copy_str(prefix, sizeof(prefix), settings, "topic_prefix");
    mm_core_set_identity(client_id, prefix);
    copy_str(s_cfg.ca_file, sizeof(s_cfg.ca_file), settings, "ca_file");
    copy_str(s_cfg.cert_set, sizeof(s_cfg.cert_set), settings, "cert_set");
    item = cJSON_GetObjectItemCaseSensitive(settings, "keepalive_s");
    s_cfg.keepalive_s =
        (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 30);
    item = cJSON_GetObjectItemCaseSensitive(settings, "connect_on");
    s_cfg.connect_on = mm_parse_connect_on(
        cJSON_IsString(item) ? item->valuestring : NULL);
    s_configured = true;
    return ESP_OK;
}

/* v1 -> v2 (the rule itself is the pure mm_migrated_connect_on, host-tested):
   a document written before connect_on existed keeps the v1 behaviour
   (any uplink); only fresh devices get the wifi default */
static esp_err_t on_migrate(uint32_t from_version, cJSON *settings)
{
    const char *add = mm_migrated_connect_on(
        from_version,
        cJSON_GetObjectItemCaseSensitive(settings, "connect_on") != NULL);

    if (add != NULL)
    {
        cJSON_AddStringToObject(settings, "connect_on", add);
    }

    return ESP_OK;
}

esp_err_t mm_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "mqtt_manager",
        .version     = 2,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
        .on_validate = on_validate,
        .on_migrate  = on_migrate,
    };

    return settings_manager_register(&DESC);
}
