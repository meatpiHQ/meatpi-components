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
 * @file mqtt_can.h
 * @brief CAN ⇄ MQTT bridge building blocks (TASK_mqtt_can.md): the
 *        `mqtt0` bridge endpoint (configurable pub/sub topics, BOTH
 *        direction gates) + the `canmqtt` translator (legacy JSON,
 *        batched). Composed via bridge_manager, e.g.
 *        {a:"can", b:"mqtt0", translator:"canmqtt"}.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register settings ("mqtt_can"). Call in the settings block. */
esp_err_t mqtt_can_init(void);

/** Register the `mqtt0` endpoint + `canmqtt` translator with
 *  bridge_manager (no-op when disabled in settings). Must run AFTER
 *  the settings boot pass and BEFORE bridge_manager_start(). */
esp_err_t mqtt_can_start(void);

typedef struct
{
    uint32_t frames_rx;       /* bus frames batched toward the broker   */
    uint32_t batches;         /* publishes handed to mqtt_manager       */
    uint32_t frames_tx;       /* broker frames injected on the bus      */
    uint32_t dropped_rx_gate; /* publishes refused: allow_rx=false      */
    uint32_t dropped_tx_gate; /* messages refused: allow_tx=false       */
    uint32_t dropped_q_full;  /* inbound chunks lost to a full bridge q */
    uint32_t parse_errors;    /* malformed tx objects                   */
} mqtt_can_stats_t;

void mqtt_can_stats(mqtt_can_stats_t *out);

#ifdef __cplusplus
}
#endif
