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
 * @file rtc_manager_private.h
 * @brief Internal contracts: the PURE time codec (host-testable) and the
 *        RX8130 chip layer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/* ---- pure time codec (rtc_manager_time.c) --------------------------------- */

/** Raw RX8130 time registers 0x10..0x16 in read order. */
#define RTC_REGS_LEN 7 /* sec min hour week day month year */

/** BCD-decode + plausibility gate (2020..2099, valid fields). The weekday
 *  register is ignored (mktime recomputes it). */
bool rtc_time_decode(const uint8_t regs[RTC_REGS_LEN], struct tm *out);

/** Encode broken-down UTC into the register image (weekday one-hot). */
void rtc_time_encode(const struct tm *in, uint8_t regs[RTC_REGS_LEN]);

/* ---- RX8130 chip layer (rtc_manager_rx8130.c) ------------------------------- */

esp_err_t rtc_rx8130_init(void);     /* attach + control-register bring-up */
esp_err_t rtc_rx8130_read(uint8_t regs[RTC_REGS_LEN]);  /* coherent read  */
esp_err_t rtc_rx8130_write(const uint8_t regs[RTC_REGS_LEN]);

/* ---- settings (rtc_manager_settings.c) -------------------------------------- */

/** Boot-applied config (written only by the settings on_apply). */
typedef struct
{
    bool     enabled;
    bool     sntp;
    char     ntp_server[64];
    char     ntp_server2[64];   /* fallback; "" = none                   */
    uint32_t sync_interval_h;
} rtcm_config_t;

/** Register the "rtc_manager" descriptor with settings_manager. */
esp_err_t rtcm_settings_register(void);

const rtcm_config_t *rtcm_settings_config(void);
bool rtcm_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/* event_manager glue (rtc_manager_events.c) */
void rtcm_events_register(void);
