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
 * @file mqtt_manager.h
 * @brief WiCAN MQTT client owner (feature component).
 *
 * Owns THE one MQTT client (esp-mqtt, managed component — un-bundled in
 * IDF v6): broker connection, reconnect, TLS, and the device's status
 * contract. Other components don't create clients — they publish through
 * this one and register topic-filter HANDLERS into it (ownership
 * inversion, Architecture §2). autopid, bridges, alerts (battery/IMU
 * events → MQTT) all build on this surface later.
 *
 * Preserved legacy on-wire contract:
 *  - status topic `<prefix>/status`, retained `{"status": "online"}` on
 *    connect and a retained LWT `{"status": "offline"}` (byte-identical
 *    payloads to the legacy firmware — dashboards keep working);
 *  - default prefix `wican/<device_id>`; keepalive 30 s; auto-reconnect
 *    every 5 s.
 *
 * Connection is network-gated (waits for DEV_STATUS_NETWORK_CONNECTED)
 * and publishes DEV_STATUS_BIT_MQTT_CONNECTED. TLS: `mqtts://` URLs use
 * the built-in certificate bundle, or a CA file from the filesystem
 * (`ca_file` setting) for private brokers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_MANAGER_MAX_HANDLERS 8

/**
 * Message delivery callback. Runs in the esp-mqtt event task — keep it
 * SHORT and copy data out (queue it to your own task for real work).
 * Payloads larger than the RX buffer (4 KB) arrive fragmented and are
 * dropped+counted in v1.
 */
typedef void (*mqtt_manager_msg_cb_t)(const char *topic,
                                      const uint8_t *data, size_t len,
                                      void *arg);

/** Register settings ("mqtt_manager") + log descriptors. No network. */
esp_err_t mqtt_manager_init(void);

/** Start the network-gated connection (no-op when disabled in settings).
 *  ESP_ERR_INVALID_STATE when unconfigured (§4.3 step 5). */
esp_err_t mqtt_manager_start(void);
esp_err_t mqtt_manager_stop(void);

/** Connected to the broker right now (mirrors the dev-status bit). */
bool mqtt_manager_connected(void);

/** The resolved topic prefix ("wican/<id>" unless overridden) — consumers
 *  build their topics on it. Valid after the settings boot pass. */
const char *mqtt_manager_topic_prefix(void);

/**
 * THREADING MODEL (any task may call anything here):
 *  - `publish()` is the DIRECT path: thread-safe (esp-mqtt's internal
 *    lock) but the socket write happens in YOUR context — it can stall
 *    milliseconds on a congested link. Fine for occasional messages
 *    (status, alerts, command responses).
 *  - `publish_async()` is the HOT path: copies into a bounded PSRAM ring
 *    and returns immediately — it NEVER blocks and never touches the
 *    network in your context; a dedicated publisher task drains the ring.
 *    Ring full / broker down → the message is dropped and counted
 *    (mqtt_manager_stats), your task keeps its deadline. CAN-rate
 *    producers use this — and BATCH upstream (many frames → one payload,
 *    the legacy JSON-array pattern): the per-publish cost, not bytes, is
 *    what limits message rate.
 */

/** Direct publish. ESP_ERR_INVALID_STATE when not connected. */
esp_err_t mqtt_manager_publish(const char *topic, const void *data,
                               size_t len, int qos, bool retain);

/** Non-blocking queued publish (see the threading model above). Topic
 *  < 128 chars, payload ≤ 4 KB. ESP_ERR_NO_MEM = ring full (counted),
 *  ESP_ERR_INVALID_STATE = not started/connected (counted). */
esp_err_t mqtt_manager_publish_async(const char *topic, const void *data,
                                     size_t len, int qos, bool retain);

typedef struct
{
    uint32_t published;      /* delivered to esp-mqtt (both paths)      */
    uint32_t dropped_full;   /* async: ring had no room                 */
    uint32_t dropped_offline;/* async: enqueued-or-drained while down   */
} mqtt_manager_stats_t;

esp_err_t mqtt_manager_stats(mqtt_manager_stats_t *out);

/**
 * Register a handler for an MQTT topic FILTER (`+`/`#` wildcards, MQTT
 * spec matching). Subscribed on (re)connect — and immediately when
 * already connected. Registrations live for the firmware's lifetime
 * (≤ MQTT_MANAGER_MAX_HANDLERS; ESP_ERR_NO_MEM when full). @p filter
 * must outlive the firmware (string literal / static).
 */
esp_err_t mqtt_manager_register_handler(const char *filter,
                                        mqtt_manager_msg_cb_t cb,
                                        void *arg);

#ifdef __cplusplus
}
#endif
