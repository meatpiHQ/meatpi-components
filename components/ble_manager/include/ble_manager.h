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
 * @file ble_manager.h
 * @brief WiCAN BLE GATT server owner (feature component).
 *
 * Rewrite of the legacy `ble.c` against the Coding Standard, preserving the
 * ON-AIR CONTRACT exactly — same services, characteristic UUIDs, security
 * model and device name, so every existing app/tool keeps working:
 *
 *  - Device Information Service (0x180A): manufacturer "MEATPI.COM", model
 *    "WiCAN-PRO", serial (derived from the device name), HW/FW/SW revision,
 *    system id, regulatory certification — all read-only.
 *  - FFF0 service (advertised): FFF1 notify/indicate = data OUT, FFF2
 *    write/write-NR = data IN (the ELM/OBD data pipe), plus the two 128-bit
 *    COMMAND-LINE characteristics (CLI OUT notify + CLI IN write with
 *    long-write support) kept for the future cmdline_manager.
 *  - Security: LE Secure Connections + MITM + bonding, static passkey
 *    (settings), IO_CAP_OUT, 16-byte keys, ENC_MITM permissions on every
 *    data characteristic, local privacy (RPA), MTU 517, runtime pairing
 *    enable/disable.
 *  - Device name: "WiC_<device id>" — the id comes from
 *    dev_status_manager_device_id() (12 hex chars of the SoftAP MAC).
 *
 * Data-path face: the standard endpoint trio (subscribe/unsubscribe/send,
 * chunk layout shared with obd_chip/socket_manager) — glue or main wraps it
 * into a bridge_manager endpoint, so OBD↔BLE / CAN↔BLE / anything↔BLE are
 * configured bridges, not code here. ONE subscriber (a BLE link is one
 * bridge endpoint).
 *
 * Coexistence note: the legacy code stopped WiFi/config server on BLE
 * connect (sideways calls). This component only publishes
 * DEV_STATUS_BIT_BLE_ENABLED/CONNECTED — any radio-coexistence policy
 * belongs to the composition root, not here (Architecture §2).
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

/* Chunk layout shared with the firmware-wide endpoint convention. */
#define BLE_MANAGER_CHUNK_SIZE 128

typedef struct
{
    uint16_t len;
    uint8_t  data[BLE_MANAGER_CHUNK_SIZE];
} ble_chunk_t;

/** Register settings ("ble_manager") + log descriptors. No radio traffic. */
esp_err_t ble_manager_init(void);

/** Bring the BLE stack up per the boot-applied settings (controller +
 *  Bluedroid + GATT tables + advertising). `enabled=false` = ESP_OK with the
 *  radio left off. ESP_ERR_INVALID_STATE if unconfigured (§4.3 step 5). */
esp_err_t ble_manager_start(void);

/** Stop advertising and shut the BLE stack down. */
esp_err_t ble_manager_stop(void);

/* ---- endpoint trio (the bridge ABI — same shape as obd_chip/sockets) ------ */

/** Attach the ONE subscriber queue (items ble_chunk_t): FFF2 writes from the
 *  client flow into it. ESP_ERR_INVALID_STATE if taken. */
esp_err_t ble_manager_subscribe(QueueHandle_t q);
esp_err_t ble_manager_unsubscribe(QueueHandle_t q);

/** TX to the client as FFF1 notifications — queued, packed to the
 *  negotiated MTU by the TX task (legacy packing/congestion logic).
 *  ESP_ERR_INVALID_STATE when no client is connected. */
esp_err_t ble_manager_send(const uint8_t *data, size_t len);

/* ---- status ----------------------------------------------------------------- */

/** True when BLE is enabled in SETTINGS — the configured truth, stable
 *  across runtime stop/start cycles (interface_manager's policy input;
 *  the BLE_ENABLED dev-status bit tracks the RUNNING state instead). */
bool ble_manager_is_enabled(void);
bool ble_manager_is_connected(void);
bool ble_manager_is_secured(void);   /* authenticated + encrypted pairing */

/* ---- pairing window (runtime, ephemeral — like the legacy toggle) ---------- */

void ble_manager_pairing_enable(void);
void ble_manager_pairing_disable(void);
bool ble_manager_pairing_is_enabled(void);

/* ---- command line characteristics (future cmdline_manager registers) -------- */

/** Handler for complete CLI lines written to CLI IN (secured links only).
 *  Runs in the BT stack callback — keep it short or hand off to a task. */
typedef void (*ble_cli_handler_t)(const char *line);

esp_err_t ble_manager_set_cli_handler(ble_cli_handler_t handler);

/** Write console output back to the client (CLI OUT notify, MTU-chunked). */
esp_err_t ble_manager_cli_write(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
