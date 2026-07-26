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
 * @file uds_manager_events.c
 * @brief event_manager glue: the `uds.request` action (fire a UDS
 *        request from a rule) and the `uds.response` event source it
 *        publishes — so rules and scripts can chain on UDS outcomes.
 *
 * Events carry OUTCOMES, not blobs (SCRIPTING.md §2): the response hex
 * is truncated to the event kv string limit (~23 bytes of payload).
 * Rules that need full payloads run a script (`script.run` — the uds()
 * binding returns up to 128 bytes into script memory). The action
 * blocks the dispatcher for the transaction (bounded by the p2, p2star
 * and max-pending caps — same contract as `http.post`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "event_manager.h"

#include "uds_manager.h"
#include "uds_proto.h"

static esp_err_t act_request(const cJSON *with, const em_event_t *ev)
{
    (void)ev;

    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(with, "tx");
    const cJSON *rx = cJSON_GetObjectItemCaseSensitive(with, "rx");
    const cJSON *reqs = cJSON_GetObjectItemCaseSensitive(with, "req");
    const cJSON *ext = cJSON_GetObjectItemCaseSensitive(with, "ext");

    if (!cJSON_IsString(tx) || !cJSON_IsString(rx) ||
        !cJSON_IsString(reqs))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uds_addr_t addr =
    {
        .tx_id  = (uint32_t)strtoul(tx->valuestring, NULL, 16),
        .rx_id  = (uint32_t)strtoul(rx->valuestring, NULL, 16),
        .ext_id = cJSON_IsTrue(ext),
    };

    uint8_t reqb[64];
    size_t reqn = 0;

    if (!uds_hex_to_bytes(reqs->valuestring, reqb, sizeof(reqb), &reqn) ||
        reqn == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t respb[512];
    size_t respn = 0;
    uds_result_t res = { 0 };

    esp_err_t err = uds_request(&addr, reqb, reqn, respb, sizeof(respb),
                                &respn, NULL, &res);

    /* publish the outcome (also on failure — rules may want timeouts) */
    char hex[48];

    if (err == ESP_OK && respn > 0)
    {
        uds_bytes_to_hex(respb, respn < 15 ? respn : 15, hex, sizeof(hex));
    }
    else
    {
        hex[0] = '\0';
    }

    em_event_t out =
    {
        .ts_us = esp_timer_get_time(),
        .n = 5,
        .kv =
        {
            em_kv_bool("ok", err == ESP_OK && !res.negative),
            em_kv_i64("nrc", (err == ESP_OK && res.negative) ? res.nrc : -1),
            em_kv_i64("len", (int64_t)respn),
            em_kv_str("data", hex),
            em_kv_str("req", reqs->valuestring),
        },
    };

    snprintf(out.source, sizeof(out.source), "uds");
    snprintf(out.name, sizeof(out.name), "response");
    (void)event_manager_publish(&out);

    return err;
}

void uds_events_register(void)
{
    static const em_key_decl_t KEYS[] =
    {
        { "ok",   EM_VAL_BOOL },
        { "nrc",  EM_VAL_I64  },
        { "len",  EM_VAL_I64  },
        { "data", EM_VAL_STR  },
        { "req",  EM_VAL_STR  },
    };
    static const em_source_decl_t SRC =
    {
        .source      = "uds",
        .name        = "response",
        .description = "outcome of a uds.request action (data truncated; "
                       "full payloads via a script's uds() binding)",
        .keys        = KEYS,
        .n_keys      = sizeof(KEYS) / sizeof(KEYS[0]),
    };
    static const em_action_t REQ =
    {
        .name = "uds.request",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"tx\":{\"type\":\"string\",\"description\":\"tx CAN id hex\"},"
            "\"rx\":{\"type\":\"string\",\"description\":\"rx CAN id hex\"},"
            "\"req\":{\"type\":\"string\",\"description\":\"request hex\"},"
            "\"ext\":{\"type\":\"boolean\"}},"
            "\"required\":[\"tx\",\"rx\",\"req\"]}",
        .run = act_request,
        .blocking = true, /* UDS/ISO-TP round-trip — off the dispatcher */
    };

    (void)event_manager_declare_source(&SRC);
    (void)event_manager_register_action(&REQ);
}
