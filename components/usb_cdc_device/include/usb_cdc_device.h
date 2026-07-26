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
 * @file usb_cdc_device.h
 * @brief WiCAN as a USB CDC-ACM (serial) DEVICE carrying the J2534 wire
 *        protocol — the `device_class=cdc` alternative to the NCM/RNDIS
 *        network device (usb_net_device).
 *
 * The PC sees a virtual COM port (inbox usbser.sys on Windows, ttyACM on
 * Linux). The same framed J2534 wire protocol that runs over TCP runs over
 * this serial link instead: this component provides a byte transport
 * (StreamBuffer-backed read + bulk-IN write) to j2534_server via
 * j2534_server_set_serial_transport(), and drives one tester session per
 * host connect with j2534_server_serve_serial().
 *
 * Ownership: usb_host_manager owns the role/mux decision and calls
 * usb_cdc_device_start() in role=device + device_class=cdc. Only one USB
 * device personality runs at a time (NCM/RNDIS XOR CDC-ACM), fixed at boot.
 *
 * RAM: the bulk RX/TX DMA buffers are INTERNAL heap (~2 KB while active);
 * the RX StreamBuffer + the session task stack are PSRAM.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool configured;   /* host enumerated + configured the device      */
    bool port_open;    /* host asserted DTR (a client is attached)     */
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} usb_cdc_device_status_t;

/** Bring up the CDC-ACM device + the serial J2534 session task. Caller
 *  has already muxed the connector to the ESP OTG. One-shot (role changes
 *  are reboot-to-apply). */
esp_err_t usb_cdc_device_start(void);

esp_err_t usb_cdc_device_get_status(usb_cdc_device_status_t *out);

#ifdef __cplusplus
}
#endif
