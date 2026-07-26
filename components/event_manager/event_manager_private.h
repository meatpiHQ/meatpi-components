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
 * @file event_manager_private.h
 * @brief Internal types + pure-module contracts shared by the .c files
 *        and the host suite. Not part of the public API.
 */
#pragma once

#include "event_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bounds (settings arrays cap at 16 per the validator; meatpi OK'd) */
#define EM_RULES_MAX   16
#define EM_TIMERS_MAX  4
#define EM_MATCH_MAX   4
#define EM_WHEN_MAX    4
#define EM_SOURCES_MAX 24
#define EM_ACTIONS_MAX 16
#define EM_VALUES_MAX  16
#define EM_WITH_MAX    384   /* serialized `with` object per rule        */
#define EM_RING        32    /* /api/events/log entries                  */

#define EM_NAME_LEN 32
#define EM_SEL_LEN  33       /* "source.name"                            */
#define EM_KEY_LEN  16

typedef enum
{
    EM_OP_EQ = 0,
    EM_OP_NE,
    EM_OP_GT,
    EM_OP_GE,
    EM_OP_LT,
    EM_OP_LE,
    EM_OP_CHANGED,
    EM_OP_CONTAINS,
} em_op_t;

typedef struct
{
    char   key[EM_KEY_LEN];
    bool   is_num;              /* number (or bool as 0/1) vs string      */
    double num;
    char   str[EM_STR_MAX];
} em_operand_t;

typedef struct
{
    char         key[EM_KEY_LEN];
    em_op_t      op;
    em_operand_t val;           /* unused for `changed`                   */
} em_when_t;

typedef struct
{
    char         name[EM_NAME_LEN];
    bool         enabled;
    char         on[EM_SEL_LEN];            /* "source.name"              */
    em_operand_t match[EM_MATCH_MAX];       /* key inside operand         */
    uint8_t      n_match;
    em_when_t    when[EM_WHEN_MAX];
    uint8_t      n_when;
    char         action[EM_NAME_LEN];       /* `do`                       */
    char         with_json[EM_WITH_MAX];    /* serialized object or ""    */
    uint32_t     cooldown_ms;
} em_rule_t;

/** Per-rule runtime state (RAM only; `changed` is per-boot by design). */
typedef struct
{
    int64_t last_fire_us;       /* 0 = never                              */
    struct
    {
        bool   valid;
        bool   is_num;
        double num;
        char   str[EM_STR_MAX];
    } last[EM_WHEN_MAX];        /* previous value per `changed` condition */
} em_rule_state_t;

/** Settings-applied timer config (the engine owns the esp_timer handles). */
typedef struct
{
    char     name[EM_KEY_LEN];
    uint32_t period_s;
} em_timer_cfg_t;

/* ---- pure: rules (event_manager_rules.c — host-tested) --------------------- */

/** Parse+shape-validate the rules array. Registry-existence checks are
 *  the caller's (they need the live registries). */
esp_err_t em_rules_parse(const cJSON *rules, em_rule_t *out, int max,
                         int *count, char *err, size_t err_len);

const em_kv_t *em_event_get(const em_event_t *ev, const char *key);

/** All `match` filters hold (typed equality). */
bool em_rule_match(const em_rule_t *r, const em_event_t *ev);

/** All `when` conditions hold (AND). Updates the `changed` slots in
 *  @p st for EVERY call (i.e. every event that passed `match`). */
bool em_rule_when(const em_rule_t *r, const em_event_t *ev,
                  em_rule_state_t *st);

/** Cooldown gate: true = allowed to fire (stamps last_fire_us). */
bool em_rule_cooldown_ok(const em_rule_t *r, em_rule_state_t *st,
                         int64_t now_us);

/* ---- pure: templates (event_manager_template.c — host-tested) --------------- */

/** Resolver for names not carried by the event (pull values): renders
 *  the value into @p out as a string. May be NULL. */
typedef esp_err_t (*em_tpl_resolver_t)(const char *name, char *out,
                                       size_t out_len);

/**
 * Substitute ${key} in @p tpl: event kv first (+ builtins `ts` = ts_us,
 * `source`, `name`), then @p resolver, else the literal `null`.
 * Event values render raw (f64 %g, i64 decimal, bool true/false,
 * strings verbatim — the template author adds JSON quotes).
 * ESP_ERR_INVALID_SIZE when @p out overflows.
 */
esp_err_t em_template_render(const char *tpl, const em_event_t *ev,
                             em_tpl_resolver_t resolver, char *out,
                             size_t out_len);

/** Render one kv value as a raw string (the template formatting rules). */
void em_kv_render(const em_kv_t *kv, char *out, size_t out_len);

#ifndef EM_HOST_TEST
/* ---- registry half (event_manager_registry.c): registries, the log
        ring, the JSON builders. The engine half (event_manager.c) owns
        the queue/dispatcher/rules/settings/timers. ----------------------- */

const em_action_t *em_find_action(const char *name);
esp_err_t em_value_resolve(const char *name, char *out, size_t out_len);
void em_ring_append(const em_event_t *ev, uint16_t fired);
int em_source_count(void);
int em_action_count(void);
int em_value_count(void);
bool em_selector_known(const char *selector);   /* "source.name"        */

/* ---- settings half (event_manager_settings.c): descriptor + the parsed
        boot-applied config. The engine half owns runtime state (rule
        states, timer handles, the stop latch), index-aligned with these. */

/** Register the "event_manager" descriptor with settings_manager. */
esp_err_t em_settings_register(void);

bool em_settings_enabled(void);        /* settings `enabled` (true pre-apply) */
bool em_settings_is_configured(void);  /* boot apply ran (standard §4.3)      */

const em_rule_t *em_settings_rules(void);        /* EM_RULES_MAX slots     */
const em_timer_cfg_t *em_settings_timer_cfg(int idx); /* NULL past end    */
int em_settings_timer_count(void);

/* rule-table introspection (also used by the registry's log builder) */
int em_rule_count(void);
const char *em_rule_name(int idx);

/* JSON builders for the http surface */
cJSON *em_core_sources_json(void);
cJSON *em_core_actions_json(void);
cJSON *em_core_values_json(void);
cJSON *em_core_log_json(void);      /* stats + the event ring            */
#endif

#ifdef __cplusplus
}
#endif
