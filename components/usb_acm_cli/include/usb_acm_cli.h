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
 * @file usb_acm_cli.h
 * @brief CDC-ACM host console — the espnetlink (LTE dongle) management
 *        interface. The dongle is a composite device: its RNDIS side is
 *        the LTE data path (usb_eth_host), its CDC-ACM side is an
 *        AT-command console (signal strength, modem status, SMS). This
 *        component binds that ACM interface via CherryUSB's usbh_cdc_acm
 *        driver and exposes it three ways:
 *          - `POST /api/usb/acm/cmd` (send a line, collect the response),
 *          - the `acm` CLI,
 *          - a bridge_manager endpoint `acm` (raw passthrough — bridge
 *            any transport to the modem console).
 *
 * The USB host itself is owned by usb_host_manager/usb_eth_host; this
 * component only registers the CDC-ACM class callbacks CherryUSB invokes
 * on attach/detach. Settings ("usb_acm_cli"): enabled (default false),
 * cli.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "usb_acm_gps.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_acm_cli_init(void);
esp_err_t usb_acm_cli_start(void);
esp_err_t usb_acm_cli_stop(void);

/** True while a CDC-ACM device is attached and bound. */
bool usb_acm_cli_connected(void);

/**
 * The last GPS fix from the dongle's periodic `gps -p -j` poll (cache
 * read — never issues USB, never blocks). @p out->valid is false when no
 * device is attached or there is no live fix; @p out->age_ms is the age
 * of a valid fix. ESP_ERR_INVALID_ARG on NULL.
 */
esp_err_t usb_acm_cli_gps_get(usb_acm_gps_t *out);

/**
 * Sink invoked after each GPS poll refresh (poll-task context) with the
 * freshly parsed fix — main wires it to the autopid publisher so GPS
 * becomes first-class autopid parameters. The sink MUST NOT block or
 * touch storage. NULL unregisters. ONE sink.
 */
typedef void (*usb_acm_gps_sink_t)(const usb_acm_gps_t *fix);
void usb_acm_cli_set_gps_sink(usb_acm_gps_sink_t sink);

/**
 * Send @p line (a trailing CR is appended if missing) to the ACM console
 * and collect the response until the dongle's `esp>` prompt, or 2 s of
 * quiet (prompt-less output), or @p timeout_ms elapses. Serialized.
 * ESP_ERR_INVALID_STATE if no device.
 */
esp_err_t usb_acm_cli_command(const char *line, char *resp, size_t resp_cap,
                              size_t *resp_len, uint32_t timeout_ms);

esp_err_t usb_acm_cli_register_http(void); /* POST /api/usb/acm/cmd, GET /api/gps */

#ifdef __cplusplus
}
#endif
