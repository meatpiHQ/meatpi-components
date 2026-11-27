/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_flash_bind_run.c
 * @brief The flash_* bindings that describe the run: its state, phase,
 *        progress, log, operator prompts and parameters. Split from
 *        script_engine_flash_bind.c for the 700-line rule; listed in its
 *        table.
 */
#include "script_engine_flash.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"

#include "script_engine_private.h"

#define FL_ERR "flash_error"

int se_fr_abort_requested(bvm *vm)
{
    be_pushbool(vm, se_flash_abort_requested());
    be_return(vm);
}

int se_fr_state(bvm *vm)
{
    const char *s = se_fl_str(vm, 1, "flash_state(\"untouched\"|\"loader\"|\"unknown\")");

    if (strcmp(s, "untouched") != 0 && strcmp(s, "loader") != 0 &&
        strcmp(s, "unknown") != 0)
    {
        be_raise(vm, FL_ERR, "state is untouched, loader or unknown");
    }

    se_flash_set_ecu_state(s);
    be_return_nil(vm);
}

int se_fr_phase(bvm *vm)
{
    se_check_budget(vm);
    se_flash_set_phase(se_fl_str(vm, 1, "flash_phase(name)"));
    be_return_nil(vm);
}

int se_fr_progress(bvm *vm)
{
    se_check_budget(vm);
    se_flash_set_progress(se_fl_int(vm, 1, 0), se_fl_int(vm, 2, 0));
    be_return_nil(vm);
}

int se_fr_log(bvm *vm)
{
    se_flash_log(se_fl_str(vm, 1, "flash_log(msg)"));
    be_return_nil(vm);
}

static bool ask_alive(void *ctx)
{
    (void)ctx;
    return !se_flash_abort_requested() && !se_run_killed();
}

int se_fr_ask(bvm *vm)
{
    se_check_budget(vm);

    const char *text = se_fl_str(vm, 1, "flash_ask(text[, choices])");

    /* the answer comes over HTTP: a synchronous run holds the server */
    if (!se_flash_run_is_async())
    {
        be_raise(vm, FL_ERR, "flash_ask needs an async run "
                 "(POST /api/scripts/run with \"async\": true)");
    }

    const char *choices = (be_top(vm) >= 2 && be_isstring(vm, 2))
                              ? be_tostring(vm, 2) : "";
    char answer[96];

    if (se_flash_ask(text, choices, ask_alive, NULL, answer,
                     sizeof(answer)) != ESP_OK)
    {
        se_check_budget(vm); /* raises when killed */
        be_raise(vm, FL_ERR, "prompt abandoned: the run was asked to stop");
    }

    be_pushstring(vm, answer);
    be_return(vm);
}

int se_fr_param(bvm *vm)
{
    const char *name = se_fl_str(vm, 1, "flash_param(name)");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(se_flash_params(), name);

    if (cJSON_IsString(v))
    {
        be_pushstring(vm, v->valuestring);
    }
    else if (cJSON_IsBool(v))
    {
        be_pushbool(vm, cJSON_IsTrue(v));
    }
    else if (cJSON_IsNumber(v))
    {
        double d = v->valuedouble;

        /* integral and in range: an int; anything else stays a real */
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 &&
            d == (double)(bint)d)
        {
            be_pushint(vm, (bint)d);
        }
        else
        {
            be_pushreal(vm, (breal)d);
        }
    }
    else
    {
        be_pushnil(vm);
    }

    be_return(vm);
}

int se_fr_micros(bvm *vm)
{
    be_pushint(vm, (bint)esp_timer_get_time());
    be_return(vm);
}
