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
 * @file mqtt_manager_events.c
 * @brief event_manager glue (the <comp>_cli.c shape): the `mqtt.rx`
 *        source and the `mqtt.publish` action.
 *
 * Hot-source rule (TASK_event_manager.md §5): mqtt.rx topics are
 * derived FROM THE ENABLED RULES at start (meatpi-approved AUTO
 * proposal) — we subscribe only what rules can match, never a
 * firehose. Payloads are truncated to the event kv size (47 chars);
 * bigger payloads belong to bridges, not automation.
 *
 * mqtt.publish rides the NEVER-BLOCKING async path; a leading "~/" in
 * the topic expands to the device prefix ("~/events" ->
 * "wican/<id>/events").
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "event_manager.h"

#include "mqtt_manager.h"
#include "mqtt_manager_private.h"

static const char *TAG = "mqtt_manager";

#define MM_EVT_TOPICS_MAX 8

static char s_topics[MM_EVT_TOPICS_MAX][64]; /* handler filters (static) */

static void rx_cb(const char *topic, const uint8_t *data, size_t len,
                  void *arg)
{
    (void)arg;

    em_event_t ev = { 0 };
    char payload[EM_STR_MAX];
    size_t n = (len < sizeof(payload) - 1) ? len : sizeof(payload) - 1;

    memcpy(payload, data, n);
    payload[n] = '\0';

    snprintf(ev.source, sizeof(ev.source), "mqtt");
    snprintf(ev.name, sizeof(ev.name), "rx");
    ev.kv[0] = em_kv_str("topic", topic);
    ev.kv[1] = em_kv_str("payload", payload);
    ev.n = 2;
    (void)event_manager_publish(&ev);
}

static esp_err_t act_publish(const cJSON *with, const em_event_t *ev)
{
    (void)ev;

    const cJSON *topic = cJSON_GetObjectItemCaseSensitive(with, "topic");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(with,
                                                            "payload");
    const cJSON *qos = cJSON_GetObjectItemCaseSensitive(with, "qos");
    const cJSON *retain = cJSON_GetObjectItemCaseSensitive(with,
                                                           "retain");

    if (!cJSON_IsString(topic) || !cJSON_IsString(payload))
    {
        return ESP_ERR_INVALID_ARG;
    }

    char full[128];
    const char *t = topic->valuestring;

    if (t[0] == '~' && t[1] == '/')
    {
        snprintf(full, sizeof(full), "%s/%s",
                 mqtt_manager_topic_prefix(), t + 2);
        t = full;
    }

    return mqtt_manager_publish_async(
        t, payload->valuestring, strlen(payload->valuestring),
        cJSON_IsNumber(qos) ? qos->valueint : 0, cJSON_IsTrue(retain));
}

void mm_events_register(void)
{
    static const em_key_decl_t RX_KEYS[] =
    {
        { "topic", EM_VAL_STR },
        { "payload", EM_VAL_STR },
    };
    static const em_source_decl_t RX =
    {
        .source = "mqtt", .name = "rx",
        .description = "message on a rule-matched topic (payload "
                       "truncated to 47 chars)",
        .keys = RX_KEYS, .n_keys = 2,
    };
    static const em_action_t PUBLISH =
    {
        .name = "mqtt.publish",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"topic\":{\"type\":\"string\",\"minLength\":1,"
                       "\"maxLength\":127},"
            "\"payload\":{\"type\":\"string\"},"
            "\"qos\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2},"
            "\"retain\":{\"type\":\"boolean\"}},"
            "\"required\":[\"topic\",\"payload\"]}",
        .run = act_publish,
    };

    (void)event_manager_declare_source(&RX);
    (void)event_manager_register_action(&PUBLISH);
}

void mm_events_start(void)
{
    /* the AUTO topic list: enabled rules' match.topic for mqtt.rx */
    int n = event_manager_rule_match_values("mqtt.rx", "topic", s_topics,
                                            MM_EVT_TOPICS_MAX);

    for (int i = 0; i < n; i++)
    {
        if (mqtt_manager_register_handler(s_topics[i], rx_cb,
                                          NULL) == ESP_OK)
        {
            ESP_LOGI(TAG, "events: subscribed rule topic '%s'",
                     s_topics[i]);
        }
    }
}
