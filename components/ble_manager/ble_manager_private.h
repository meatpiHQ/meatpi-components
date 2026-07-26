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
 * @file ble_manager_private.h
 * @brief Internal API between ble_manager translation units, incl. the pure
 *        layer (host-testable — no BT stack deps).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLM_SEND_BUF_SIZE  490  /* legacy BLE_SEND_BUF_SIZE               */
#define BLM_CLI_MAX        1024 /* legacy BLE_CMDLINE_MAX                 */
#define BLM_NAME_MAX       32
#define BLM_TX_QUEUE_DEPTH 32

/* ---- pure helpers (ble_manager_pack.c — host-tested) ------------------------ */

/** Packets needed to flush `pending` buffered bytes plus `add` new ones at
 *  `max_data` bytes per packet (legacy round-up math). */
int blm_pack_packets_needed(size_t pending, size_t add, size_t max_data);

/**
 * Legacy fill step: append from in[consumed..] into buf (current fill
 * *buf_len, capacity max_data). Advances *consumed and *buf_len.
 * Returns true when buf is full and must be flushed.
 */
bool blm_pack_fill(uint8_t *buf, size_t *buf_len, size_t max_data,
                   const uint8_t *in, size_t in_len, size_t *consumed);

/** Device name from the device id: "WiC_<id>" (legacy ble_uid). */
void blm_ident_name(const char *device_id, char *name, size_t name_len);

/** Serial number from the device name — legacy rule verbatim:
 *  serial = dev_name + 7 (the on-air value existing tools see). */
void blm_ident_serial(const char *dev_name, char *serial, size_t serial_len);

/** Map a dBm setting to the nearest supported ESP32-S3 level (-12..+9 in
 *  3 dB steps, legacy switch). Returns the clamped dBm actually used. */
int blm_ident_clamp_tx_power(int dbm);

/** Map the conn_profile setting to the connection window the peripheral
 *  REQUESTS on connect (units of 1.25 ms): "ios" (default, legacy
 *  20–40 ms) or "android_fast" (7.5–15 ms, max performance). The central
 *  decides whether to honor the request. */
void blm_ident_conn_window(const char *profile, uint16_t *min_units,
                           uint16_t *max_units);

/* ---- settings (ble_manager_settings.c) --------------------------------------- */

typedef struct
{
    bool     enabled;
    bool     pairing_at_boot;
    bool     bonding;          /* v2: keep long-term keys (reconnect w/o
                                  re-pairing); false = per-session only */
    bool     sc_only;          /* v3: require LE Secure Connections;
                                  refuses the legacy-pairing downgrade */
    uint32_t passkey;
    int      tx_power_dbm;
    uint16_t conn_min_units;   /* requested connection window, 1.25 ms units */
    uint16_t conn_max_units;
} blm_config_t;

/** Register the "ble_manager" descriptor with settings_manager. */
esp_err_t blm_settings_register(void);

/** Boot-applied config (filled by on_apply). */
const blm_config_t *blm_core_config(void);

bool blm_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/* ---- core bridges (ble_manager.c) ------------------------------------------- */

void blm_core_on_rx(const uint8_t *data, size_t len); /* FFF2 -> subscriber */
void blm_core_on_cli_line(const char *line);          /* CLI IN -> handler  */
void blm_core_on_connect(void);
void blm_core_on_disconnect(void);
bool blm_core_pairing_allowed(void);

/* ---- GATT layer (ble_manager_gatt.c) ----------------------------------------- */

esp_err_t blm_gatt_stack_up(const char *dev_name);    /* controller..adv    */
void      blm_gatt_stack_down(void);
bool      blm_gatt_connected(void);
bool      blm_gatt_secured(void);
uint16_t  blm_gatt_max_data(void);                    /* min(490, MTU-3)    */
bool      blm_gatt_congested(void);
int       blm_gatt_free_packets(void);
esp_err_t blm_gatt_notify_data(const uint8_t *buf, uint16_t len); /* FFF1   */
esp_err_t blm_gatt_notify_cli(const uint8_t *buf, uint16_t len);  /* CLIOUT */
void      blm_gatt_allow_pairing(bool allow);         /* + late encryption  */

/* ---- IO layer (ble_manager_io.c) ---------------------------------------------- */

esp_err_t blm_io_start(void);
void      blm_io_stop(void);
esp_err_t blm_io_queue_tx(const uint8_t *data, size_t len);
esp_err_t blm_io_cli_write(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
