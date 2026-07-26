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
 * @file usb_host_manager_private.h
 * @brief Internals: the PURE presence debouncer (host-tested) + glue.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_LINUX /* host test builds only the pure section */
#include "esp_netif.h"
#endif

#include "usb_host_manager.h"

/* ---- usb_host_manager_presence.c — PURE (host-tested) --------------------- */

/* ID-pin debouncer (the external_storage detect pattern): presence =
 * level LOW; an edge is reported only after N consecutive samples
 * agree, so cable bounce and EMI blips never flap the host stack. */
#define UHM_PRESENCE_STABLE_SAMPLES 3

typedef enum
{
    UHM_EDGE_NONE = 0,
    UHM_EDGE_ATTACH,
    UHM_EDGE_DETACH,
} uhm_edge_t;

typedef struct
{
    bool    present;   /* debounced state                       */
    bool    candidate; /* raw state being counted               */
    uint8_t count;
} uhm_presence_t;

void uhm_presence_init(uhm_presence_t *p, int initial_level);
uhm_edge_t uhm_presence_sample(uhm_presence_t *p, int level);

/* ---- glue ------------------------------------------------------------------ */

void uhm_events_register(void);
void uhm_events_device(bool attached);
void uhm_events_eth(bool connected, const char *ip);

/* ---- settings (usb_host_manager_settings.c) --------------------------------- */

/** Register the "usb_host_manager" descriptor with settings_manager. */
esp_err_t uhm_settings_register(void);

bool uhm_settings_enabled(void);          /* host stack runs (host role)     */
bool uhm_settings_role_device(void);      /* enabled && role=="device"       */
const char *uhm_settings_device_class(void); /* "ncm" | "rndis" | "cdc"      */
bool uhm_settings_prefer_usb_route(void);
bool uhm_settings_ip_static(void);
#ifndef CONFIG_IDF_TARGET_LINUX
const esp_netif_ip_info_t *uhm_settings_static_ip(void);
#endif
bool uhm_settings_is_configured(void);    /* boot apply ran (standard §4.3)  */

/** Mirror the applied `enabled` into the runtime status struct
 *  (usb_host_manager.c owns it); called by on_apply. */
void uhm_status_set_enabled(bool enabled);
