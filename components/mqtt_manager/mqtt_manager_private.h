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
 * @file mqtt_manager_private.h
 * @brief Internal contract: the PURE topic/url logic (host-testable).
 */
#pragma once

#include <stdbool.h>

/** MQTT-spec filter match: `+` = exactly one level, `#` = rest (last
 *  level only, also matches the parent), `$`-topics never match a
 *  wildcard FIRST level. Malformed filters (`#` not last, `+`/`#`
 *  embedded in a level) match nothing. */
bool mm_topic_matches(const char *filter, const char *topic);

/** "mqtt://host[:port]" or "mqtts://host[:port]" with a non-empty host. */
bool mm_url_valid(const char *url);

/* ---- PURE async-queue item codec (mqtt_manager_item.c) ----------------------
 * One contiguous ring item: header + NUL-terminated topic + payload, so
 * the publisher task hands esp-mqtt the topic string in place. */

#include <stddef.h>
#include <stdint.h>

#define MM_ITEM_TOPIC_MAX 128  /* incl. NUL                              */
#define MM_ITEM_DATA_MAX  4096 /* == the client's out-buffer size        */

typedef struct
{
    uint16_t topic_len; /* incl. NUL                                     */
    uint16_t data_len;
    uint8_t  qos;
    uint8_t  retain;
} mm_item_hdr_t;

/** Bytes a packed item occupies (0 = out of bounds / invalid). */
size_t mm_item_size(size_t topic_strlen, size_t data_len);

/** Pack into @p buf (>= mm_item_size). False on bounds violations. */
bool mm_item_pack(uint8_t *buf, size_t buf_len, const char *topic,
                  const void *data, size_t data_len, int qos, bool retain);

/** Borrow views into a packed item. False on a corrupt/truncated item. */
bool mm_item_unpack(const uint8_t *buf, size_t buf_len,
                    mm_item_hdr_t *hdr, const char **topic,
                    const uint8_t **data);

/* event_manager glue (mqtt_manager_events.c): mqtt.rx source (topics
 * derived from enabled rules at start) + mqtt.publish action. */
void mm_events_register(void);
void mm_events_start(void);

/* ---- settings (mqtt_manager_settings.c) -------------------------------------
 * Boot-applied broker config (standard §4.1). The identity pair
 * (client_id/topic_prefix) lives in mqtt_manager.c instead: start()
 * resolves its device-id defaults in place. */

#include "esp_err.h"

typedef struct
{
    bool     enabled;
    char     url[128];
    char     username[64];
    char     password[64];
    char     ca_file[128];     /* "" = built-in bundle                    */
    char     cert_set[25];     /* CERT_MANAGER_NAME_MAX + 1 (asserted in
                                  mqtt_manager.c — no cert_manager.h here:
                                  the host suite includes this header)    */
    uint32_t keepalive_s;
} mm_config_t;

/** Register the "mqtt_manager" descriptor with settings_manager. */
esp_err_t mm_settings_register(void);

/** Boot-applied config; valid once mm_settings_is_configured() (§4.3). */
const mm_config_t *mm_settings_config(void);
bool mm_settings_is_configured(void);

/** on_apply -> the runtime identity buffers (mqtt_manager.c). */
void mm_core_set_identity(const char *client_id, const char *prefix);
