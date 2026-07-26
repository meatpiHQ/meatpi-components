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
 * @file rtc_manager.h
 * @brief WiCAN battery-backed RTC owner (service component).
 *
 * Owns the RX8130CE RTC (shared bus via i2c_bus). The chip's backup caps
 * keep time through reboots and short unpowered periods, so:
 *
 *  - at start(): if the RTC holds a plausible time, the SYSTEM clock is
 *    restored from it (timestamps are sane seconds after power-on, no
 *    network needed) and DEV_STATUS_BIT_TIME_SYNCED is set;
 *  - when the network comes up (and sntp is enabled in settings): SNTP
 *    syncs the system clock, the RTC is written back, TIME_SYNCED is set;
 *    resync every 24 h while the network stays up.
 *
 * Time is kept in UTC everywhere (RTC and system clock); timezone is a
 * presentation concern of the consumer (legacy's worldtimeapi.org lookup
 * is intentionally gone). Thread safety: chip access serializes on an
 * internal mutex; reads use the coherent double-read pattern.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register settings ("rtc_manager") + log descriptors. No bus traffic. */
esp_err_t rtc_manager_init(void);

/** Bring up the chip, restore system time if the RTC is plausible, start
 *  the SNTP task (if enabled). ESP_ERR_INVALID_STATE when unconfigured. */
esp_err_t rtc_manager_start(void);
esp_err_t rtc_manager_stop(void);

/** Read the RTC as UTC broken-down time. ESP_ERR_INVALID_ARG when the
 *  stored time is implausible (fresh board, drained caps). */
esp_err_t rtc_manager_get_time(struct tm *out);

/** Write the RTC from the current SYSTEM clock (the one sanctioned write
 *  path — set the system clock first, then persist it here). */
esp_err_t rtc_manager_sync_from_system(void);

/** Set BOTH clocks from an epoch (UTC seconds): system time, then the
 *  RTC, then TIME_SYNCED. The manual path for when no NTP is reachable
 *  (AP mode — the browser knows the time; POST /api/rtc uses this).
 *  ESP_ERR_INVALID_ARG outside 2020..2099. */
esp_err_t rtc_manager_set_time(time_t epoch);

/** Register GET/POST /api/rtc (composition root calls it in HTTP
 *  builds). */
esp_err_t rtc_manager_register_http(void);

/** Register the `rtc` CLI command with cmdline_manager. Called
 *  INTERNALLY on the settings boot apply when the `cli` setting is true
 *  (default) — main no longer wires it. */
esp_err_t rtc_manager_register_cli(void);

/** "YYYY-MM-DDTHH:MM:SSZ" from the SYSTEM clock (UTC). Needs ≥ 21 bytes. */
esp_err_t rtc_manager_now_iso8601(char *buf, size_t len);

/** True once the system clock was set from a trusted source this boot
 *  (RTC restore, SNTP, or manual). Mirrors DEV_STATUS_BIT_TIME_SYNCED. */
bool rtc_manager_time_valid(void);

/** Blocking on-demand SNTP sync (primary + fallback server, ≤~32 s
 *  worst case). ESP_ERR_INVALID_STATE when offline/not started;
 *  ESP_FAIL when no server answered. POST /api/rtc/sync uses this. */
esp_err_t rtc_manager_sync_now(void);

/** Epoch of the last successful SNTP sync (0 = none this boot). */
time_t rtc_manager_last_sync(void);

/** The configured primary NTP server / whether SNTP is enabled. */
const char *rtc_manager_ntp_server(void);
bool rtc_manager_sntp_enabled(void);

#ifdef __cplusplus
}
#endif
