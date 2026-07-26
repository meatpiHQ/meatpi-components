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
 * @file script_engine_events.c
 * @brief event_manager glue: the `script.run {name}` action — run a
 *        stored /data/scripts script when a rule fires. Rules may also
 *        use the sugar body `{"on":"…","script":"name"}` (the parser
 *        rewrites it to this action).
 *
 * The action blocks the dispatcher while the script runs (same
 * fire-and-forget contract as `http.post`, which blocks on TLS): the VM
 * executes on script_engine's own runner task, the dispatcher just
 * waits, bounded by max_runtime_ms. Keep event-triggered scripts short;
 * long diagnostics belong behind a manual /api/scripts/run. The trigger
 * event is exposed to the script as evt_source / evt_name / evt_<key>
 * globals. Script output goes to the log (no capture buffer).
 */
#include "event_manager.h"

#include "script_engine.h"
#include "script_engine_private.h"

static esp_err_t act_run(const cJSON *with, const em_event_t *ev)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(with, "name");

    if (!cJSON_IsString(name) || !se_script_name_ok(name->valuestring))
    {
        return ESP_ERR_INVALID_ARG;
    }

    se_set_trigger(ev);

    esp_err_t err = script_engine_run_file(name->valuestring, NULL, 0);

    se_set_trigger(NULL);
    return err;
}

void se_events_register(void)
{
    /* the conventional script-emitted event (the emit() binding uses the
     * fixed kv key "value"); declared so rules can select on it */
    static const em_key_decl_t DONE_KEYS[] =
    {
        { "value", EM_VAL_STR },
    };
    static const em_source_decl_t DONE =
    {
        .source      = "script",
        .name        = "done",
        .description = "emitted by a Berry script via emit('script',"
                       "'done','value',…)",
        .keys        = DONE_KEYS,
        .n_keys      = sizeof(DONE_KEYS) / sizeof(DONE_KEYS[0]),
    };
    static const em_action_t RUN =
    {
        .name = "script.run",
        .params_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"minLength\":1,"
            "\"description\":\"script in /data/scripts (.be optional)\"}},"
            "\"required\":[\"name\"]}",
        .run = act_run,
    };

    (void)event_manager_declare_source(&DONE);
    (void)event_manager_register_action(&RUN);
}
