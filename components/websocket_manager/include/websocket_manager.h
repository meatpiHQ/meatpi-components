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
 * @file websocket_manager.h
 * @brief WiCAN WebSocket channel owner (service component).
 *
 * Owns N named WebSocket CHANNELS (settings-defined, up to 4) served by
 * THE one httpd — it registers `/ws/...` routes with http_server_manager
 * (ownership inversion; it never starts a server). Each channel presents
 * the firmware's standard chunk-stream face (queue RX via subscribe,
 * send() TX), so a channel plugs into bridge_manager as an endpoint with
 * three-line glue: OBD↔WS, CAN↔WS, the command line over WS — all
 * configured bridges, no per-use code here.
 *
 * Semantics (mirroring socket_manager):
 *  - Multi-client AGGREGATE per channel: send() fans out one WS frame to
 *    every connected client; frames from any client merge into the one
 *    subscriber queue. Per-client addressing is out of scope v1.
 *  - max_clients per channel: excess upgrades are refused at handshake.
 *  - Dead clients are reaped on send failure / fd state; the channel and
 *    its siblings keep running.
 *  - Reboot-to-apply: channels are registered once at start() from the
 *    boot-applied settings.
 *
 * Lifecycle: init (settings descriptor) between http_server_manager_init()
 * and settings_manager_start(); start() BEFORE http_server_manager_start()
 * (routes are buffered until the server starts — main's order already
 * guarantees this).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBSOCKET_MANAGER_MAX_CHANNELS 6 /* was 4; +ws_log with headroom
                                            (§12) since settings v2 */
#define WEBSOCKET_MANAGER_MAX_CLIENTS  4 /* per channel */

/* Chunk layout shared with the firmware-wide endpoint convention. */
#define WEBSOCKET_MANAGER_CHUNK_SIZE 128

typedef struct
{
    uint16_t len;
    uint8_t  data[WEBSOCKET_MANAGER_CHUNK_SIZE];
} websocket_chunk_t;

typedef struct
{
    uint16_t clients;
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t bytes_in;
    uint32_t bytes_out;
    uint32_t rx_drops;   /* subscriber queue full                    */
    uint32_t tx_drops;   /* client send failed -> client reaped     */
    uint32_t refused;    /* upgrades refused at max_clients          */
} websocket_stats_t;

/** Register the settings ("websocket_manager") + log descriptors. */
esp_err_t websocket_manager_init(void);

/** Register the enabled channels' /ws routes with http_server_manager
 *  (call before its start). ESP_ERR_INVALID_STATE when unconfigured. */
esp_err_t websocket_manager_start(void);
esp_err_t websocket_manager_stop(void);

/** Attach the ONE subscriber queue of @p channel (items
 *  websocket_chunk_t). ESP_ERR_INVALID_STATE if taken. */
esp_err_t websocket_manager_subscribe(const char *channel, QueueHandle_t q);
esp_err_t websocket_manager_unsubscribe(const char *channel,
                                        QueueHandle_t q);

/** TX: one WS frame (channel's configured binary/text mode) fanned out to
 *  every connected client of the channel. */
esp_err_t websocket_manager_send(const char *channel, const uint8_t *data,
                                 size_t len);

/** Per-channel counters. ESP_ERR_NOT_FOUND for unknown names. */
esp_err_t websocket_manager_stats(const char *channel,
                                  websocket_stats_t *out);

/** Configured channel name for slot @p idx; NULL past the configured
 *  count. Valid after the settings boot pass (bridge_endpoints registers
 *  a jack per configured channel, whatever its name); the pointer stays
 *  valid for the boot (reboot-to-apply config). */
const char *websocket_manager_channel_name(int idx);

#ifdef __cplusplus
}
#endif
