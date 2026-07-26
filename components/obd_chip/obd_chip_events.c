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
 * @file obd_chip_events.c
 * @brief event_manager glue: the `obd.request {cmd}` action (meatpi's
 *        original "MQTT message triggers an OBD request" example) and
 *        the `obd.response {cmd, response}` event it publishes back —
 *        chain a second rule on obd.response to deliver the answer
 *        (e.g. mqtt.publish ${response}).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "event_manager.h"

#include "obd_chip.h"

static esp_err_t act_request(const cJSON *with, const em_event_t *ev)
{
    (void)ev;

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(with, "cmd");

    if (!cJSON_IsString(cmd) || cmd->valuestring[0] == '\0' ||
        strlen(cmd->valuestring) > 23)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char resp[256];
    esp_err_t err = obd_chip_request(cmd->valuestring, resp,
                                     sizeof(resp),
                                     pdMS_TO_TICKS(1500));

    if (err != ESP_OK)
    {
        return err;    /* busy (monitor/update) or timeout — counted   */
    }

    /* strip trailing CR/LF for a clean single-line event value */
    size_t n = strlen(resp);

    while (n > 0 && (resp[n - 1] == '\r' || resp[n - 1] == '\n'))
    {
        resp[--n] = '\0';
    }

    em_event_t out = { 0 };

    snprintf(out.source, sizeof(out.source), "obd");
    snprintf(out.name, sizeof(out.name), "response");
    out.kv[0] = em_kv_str("cmd", cmd->valuestring);
    out.kv[1] = em_kv_str("response", resp);   /* truncated at 47      */
    out.n = 2;
    (void)event_manager_publish(&out);
    return ESP_OK;
}

void oc_events_register(void)
{
    static const em_key_decl_t KEYS[] =
    {
        { "cmd", EM_VAL_STR },
        { "response", EM_VAL_STR },
    };
    static const em_source_decl_t RESPONSE =
    {
        .source = "obd", .name = "response",
        .description = "answer to an obd.request action (response "
                       "truncated to 47 chars)",
        .keys = KEYS, .n_keys = 2,
    };
    static const em_action_t REQUEST =
    {
        .name = "obd.request",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"cmd\":{\"type\":\"string\",\"minLength\":1,"
            "\"maxLength\":23}},\"required\":[\"cmd\"]}",
        .run = act_request,
        .blocking = true, /* OBD chip round-trip — off the dispatcher */
    };

    (void)event_manager_declare_source(&RESPONSE);
    (void)event_manager_register_action(&REQUEST);
}
