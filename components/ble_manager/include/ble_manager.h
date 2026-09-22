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

/* ---- stream channels (ble_http, ble_j2534, ...) ------------------------------
 * A channel = one IN characteristic (write + write-no-rsp, ENC|AUTHEN) and
 * one OUT characteristic (indicate and/or notify, the owner decides which
 * it allows) appended to the FFF0 service, 16-bit UUIDs on the FFF0 base
 * (odd = device -> app, even = app -> device, like FFF1/FFF2). Byte-stream
 * semantics: BLE adds no framing, the owner's protocol does. Registration
 * is PRE-START only (the GATT table is built at ble_manager_start()); the
 * registry survives runtime stop/start cycles (interface_manager). Bounded
 * registry (standard §12). Documented for app developers in BLE_API.md;
 * NimBLE backend only.
 *
 * OUT mode (2026-09-22): the APP picks it with the CCCD it writes on the
 * OUT characteristic. Indications = one PDU in flight, confirmed end to
 * end, no app-side flow control needed, slow (one 490 B unit per two
 * connection intervals). Notifications = the controller packs several
 * PDUs per connection event (2x-10x faster, PHY-scaled), the owner's
 * protocol MUST carry app-side credits and a per-frame counter: the LL is
 * reliable, but a stack can drop a PDU it already accepted (measured: the
 * controller dropped notifications when its heap-allocated ACL TX buffer
 * failed, fixed with CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=12; a phone
 * stack can drop when it is slow). A channel registered with out_modes 0
 * or INDICATE only keeps today's contract. */

#define BLE_MANAGER_CHANNEL_MAX       4    /* bounded registry            */
#define BLE_MANAGER_CHANNEL_WRITE_MAX 490  /* one ATT write from the app  */
#define BLE_MANAGER_CHANNEL_RX_MIN    2048 /* smallest accepted rx_size   */

/* OUT direction modes: a bitmask in the descriptor (what the owner
   allows) and the single live value the central selected via its CCCD */
#define BLE_MANAGER_CH_OUT_NONE       0    /* not subscribed              */
#define BLE_MANAGER_CH_OUT_INDICATE   1    /* CCCD 0x0002                 */
#define BLE_MANAGER_CH_OUT_NOTIFY     2    /* CCCD 0x0001 (wins if both)  */

typedef enum
{
    BLE_MANAGER_CH_CONNECTED = 0,  /* link up, NOT yet secured: unusable  */
    BLE_MANAGER_CH_SECURED,        /* enc + authen: bytes flow from here  */
    BLE_MANAGER_CH_DISCONNECTED,   /* link down: reads return <0 once     */
} ble_manager_channel_event_t;

/** Runs in the BT host task: set a flag / give a notification, never
 *  block and never call back into ble_manager. */
typedef void (*ble_manager_channel_event_cb_t)(int id,
                                               ble_manager_channel_event_t evt,
                                               void *arg);

typedef struct
{
    const char *name;        /* "http", "j2534" - status/CLI label        */
    uint16_t    uuid_out;    /* 16-bit on the FFF0 base, notify           */
    uint16_t    uuid_in;     /* 16-bit on the FFF0 base, write            */
    uint8_t    *rx_storage;  /* caller-owned StreamBuffer storage (PSRAM) */
    size_t      rx_size;     /* >= BLE_MANAGER_CHANNEL_RX_MIN             */
    ble_manager_channel_event_cb_t on_event; /* nullable                  */
    void       *arg;
    uint8_t     out_modes;   /* BLE_MANAGER_CH_OUT_* mask the owner allows;
                                0 = INDICATE only (the pre-2026-09-22
                                contract). The GATT properties follow it. */
} ble_manager_channel_desc_t;

typedef struct
{
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_overflow;    /* writes (partially) dropped: buffer full   */
    uint32_t tx_timeouts;    /* notify credit waits that expired          */
    uint32_t tx_link_down;   /* writes refused: no secured link           */
    uint32_t tx_notifications; /* PDUs sent as notifications              */
    uint32_t tx_indications; /* PDUs sent (and confirmed) as indications  */
    size_t   rx_pending;     /* bytes waiting in the StreamBuffer         */
    uint8_t  out_mode;       /* live BLE_MANAGER_CH_OUT_* (0 = unsubscribed) */
} ble_manager_channel_stats_t;

/** ESP_ERR_INVALID_STATE after start; ESP_ERR_NO_MEM when the registry is
 *  full (logged E, §12); ESP_ERR_INVALID_ARG on a bad descriptor or a
 *  UUID already in use (incl. FFF1/FFF2). */
esp_err_t ble_manager_channel_register(const ble_manager_channel_desc_t *desc,
                                       int *out_id);

/** Read up to n bytes, blocking up to timeout_ms. Returns bytes read
 *  (>0), 0 on timeout, <0 when the link is down or the id is unknown -
 *  the j2534 transport read contract. Bytes left over from a previous
 *  link are discarded on the first read after it dropped. One reader
 *  task per channel. */
int ble_manager_channel_read(int id, uint8_t *buf, size_t n,
                             uint32_t timeout_ms);

/** Write n bytes as MTU-sized PDUs in the OUT mode the central selected
 *  (ble_manager_channel_out_mode): indications are confirmed one at a
 *  time, notifications are queued with a bounded ENOMEM retry; both wait
 *  up to BLM_CHANNEL_TX_WAIT_MS per PDU. Returns n, or <0: -1 no secured
 *  link / unknown id, -2 credit timeout (a prefix may have been sent).
 *  Never call from the BT host task. */
int ble_manager_channel_write(int id, const uint8_t *buf, size_t n);

/** The live OUT mode of channel id (BLE_MANAGER_CH_OUT_*): what the
 *  central's CCCD selected among the owner's out_modes; INDICATE when it
 *  has not subscribed (writes still go out as indications, as before). */
uint8_t ble_manager_channel_out_mode(int id);

/** "none" | "indicate" | "notify" (status / logs). */
const char *ble_manager_channel_out_name(uint8_t mode);

/** Largest payload of one OUT PDU on the current link: min(490, MTU-3).
 *  20 when nothing is connected. */
uint16_t ble_manager_channel_pdu_max(void);

esp_err_t ble_manager_channel_stats(int id, ble_manager_channel_stats_t *out);

/** Registry occupancy for WICAN CAPS / /api/status health.caps (§12). */
void ble_manager_channel_capacity(size_t *used, size_t *cap);

/** GET /api/ble: link state + the channel registry with its counters
 *  (components/HTTP_API.md 6e15). Call before http_server_manager_start(). */
esp_err_t ble_manager_register_http(void);

#ifdef __cplusplus
}
#endif
