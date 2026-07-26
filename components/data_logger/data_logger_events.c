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
 * @file data_logger_events.c
 * @brief event_manager glue: the `logger.rotated {file}` /
 *        `logger.error {count}` sources, the `logger.enable` /
 *        `logger.disable` gate actions (meatpi 2026-07-07: always-on
 *        when enabled, plus rule-driven gating), and the
 *        `logger.write` action — THE producer path (autopid Phase 6):
 *        a rule routes any event's values into the log, e.g.
 *          match autopid.param ->
 *            logger.write {"source":"autopid","name":"${param}",
 *                          "value":"${value}"}
 *        so autopid stays ignorant of data_logger and users pick what
 *        gets logged. All handlers are dispatcher-safe: gate = a bool,
 *        write = registry lookup + ring push (never blocks, no IO).
 */
#include <stdio.h>
#include <stdlib.h>

#include "event_manager.h"

#include "data_logger_private.h"

static esp_err_t act_enable(const cJSON *with, const em_event_t *ev)
{
    (void)with;
    (void)ev;
    dl_runtime_gate(true);
    return ESP_OK;
}

static esp_err_t act_disable(const cJSON *with, const em_event_t *ev)
{
    (void)with;
    (void)ev;
    dl_runtime_gate(false);
    return ESP_OK;
}

static esp_err_t act_write(const cJSON *with, const em_event_t *ev)
{
    (void)ev;

    const cJSON *source = cJSON_GetObjectItemCaseSensitive(with,
                                                           "source");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(with, "name");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(with,
                                                          "value");

    if (!cJSON_IsString(name) || name->valuestring[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    double v;

    if (cJSON_IsNumber(value))
    {
        v = value->valuedouble;
    }
    else if (cJSON_IsString(value)) /* ${...} templates render strings */
    {
        char *end = NULL;

        v = strtod(value->valuestring, &end);

        if (end == value->valuestring)
        {
            return ESP_ERR_INVALID_ARG; /* not numeric */
        }
    }
    else
    {
        return ESP_ERR_INVALID_ARG;
    }

    dl_param_t param;
    esp_err_t err = data_logger_register_param(
        cJSON_IsString(source) ? source->valuestring : "events",
        name->valuestring, &param);

    if (err != ESP_OK)
    {
        return err; /* registry full or bad names */
    }

    return data_logger_write(param, v);
}

void dl_events_register(void)
{
    static const em_key_decl_t ROTATED_KEYS[] =
    {
        { "file", EM_VAL_STR },
    };
    static const em_source_decl_t ROTATED =
    {
        .source = "logger", .name = "rotated",
        .description = "a log file reached its size cap and a new one "
                       "was opened; {file} is the finished file",
        .keys = ROTATED_KEYS, .n_keys = 1,
    };
    static const em_key_decl_t ERROR_KEYS[] =
    {
        { "count", EM_VAL_F64 },
    };
    static const em_source_decl_t ERROR =
    {
        .source = "logger", .name = "error",
        .description = "a storage write failed (card yanked / full / "
                       "corrupt); the writer recovers on its own",
        .keys = ERROR_KEYS, .n_keys = 1,
    };

    static const em_action_t ENABLE =
    {
        .name = "logger.enable",
        .params_schema = "{\"type\":\"object\",\"properties\":{}}",
        .run = act_enable,
    };
    static const em_action_t DISABLE =
    {
        .name = "logger.disable",
        .params_schema = "{\"type\":\"object\",\"properties\":{}}",
        .run = act_disable,
    };
    static const em_action_t WRITE =
    {
        .name = "logger.write",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"source\":{\"type\":\"string\",\"maxLength\":15,"
            "\"description\":\"record namespace (default: events)\"},"
            "\"name\":{\"type\":\"string\",\"maxLength\":31},"
            "\"value\":{\"type\":\"string\","
            "\"description\":\"numeric or a ${key} template\"}},"
            "\"required\":[\"name\",\"value\"]}",
        .run = act_write,
    };

    (void)event_manager_declare_source(&ROTATED);
    (void)event_manager_declare_source(&ERROR);
    (void)event_manager_register_action(&ENABLE);
    (void)event_manager_register_action(&DISABLE);
    (void)event_manager_register_action(&WRITE);
}

void dl_events_rotated(const char *file)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "logger");
    snprintf(ev.name, sizeof(ev.name), "rotated");
    ev.kv[0] = em_kv_str("file", file);
    ev.n = 1;
    (void)event_manager_publish(&ev);
}

void dl_events_error(const char *what, int count)
{
    (void)what;

    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "logger");
    snprintf(ev.name, sizeof(ev.name), "error");
    ev.kv[0] = em_kv_f64("count", (double)count);
    ev.n = 1;
    (void)event_manager_publish(&ev);
}
