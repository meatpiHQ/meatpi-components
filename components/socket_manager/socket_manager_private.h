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
 * @file socket_manager_private.h
 * @brief Internal API between socket_manager translation units, and the pure
 *        policy layer (host-testable — no lwIP dependency).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure policy (socket_manager_policy.c — host-tested) ------------------- */

/** Listener retry backoff progression: 1 s → 2 s → 4 s → 8 s (cap).
 *  prev_ms == 0 (fresh failure) yields the first step. */
uint32_t smp_backoff_next_ms(uint32_t prev_ms);

typedef enum
{
    SMP_ACCEPT = 0,        /* take the client                         */
    SMP_REJECT,            /* at max_clients: accept-then-close (§2)  */
} smp_accept_verdict_t;

smp_accept_verdict_t smp_accept_decision(int connected_count, int max_clients);

/**
 * Validate the "servers" array (already schema-validated for types/ranges):
 * cross-item rules — unique names, unique ports among enabled servers,
 * max_clients within the static table. Pure; used by on_validate.
 */
esp_err_t smp_validate_servers(const cJSON *servers, char *err,
                               size_t err_len);

/* ---- parsed per-server config (filled by on_apply) -------------------------- */

typedef struct
{
    bool     enabled;
    bool     is_udp;
    char     name[16];
    uint16_t port;
    uint8_t  max_clients;
    uint16_t keepalive_s;
} smp_server_cfg_t;

/** Parse one settings array item into @p out (code defaults for optional
 *  keys: max_clients=2, keepalive_s=30, enabled=false). Pure. */
esp_err_t smp_parse_server(const cJSON *item, smp_server_cfg_t *out);

/* ---- net layer (socket_manager_net.c) — indexed like the config array ------ */

esp_err_t sm_net_start(void);
void      sm_net_stop(void);
esp_err_t sm_net_send(int idx, const uint8_t *data, size_t len);
esp_err_t sm_net_subscribe(int idx, void *queue_handle);
esp_err_t sm_net_unsubscribe(int idx, void *queue_handle);
esp_err_t sm_net_stats(int idx, void *stats_out);

/* ---- settings (socket_manager_settings.c) ----------------------------------- */

/** Register the "socket_manager" descriptor with settings_manager. */
esp_err_t sm_settings_register(void);

/** Boot-applied config for slot @p idx; NULL past the configured count. */
const smp_server_cfg_t *sm_core_config(int idx);

int  sm_settings_count(void);         /* configured server slots           */
bool sm_settings_is_configured(void); /* boot apply ran (standard §4.3)    */

#ifdef __cplusplus
}
#endif
