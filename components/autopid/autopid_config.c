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
 * @file autopid_config.c
 * @brief The PID/filter tables: JSON parse/validate (PURE, host-tested).
 *        The file itself (/data/autopid/config.json: load, the repairs
 *        at load, save) is autopid_config_file.c, target only.
 *
 * The tables live in a FILE, not settings: vehicle profiles carry far
 * more than the settings validator's 16-item array cap, and that cap is
 * right for settings (TASK_autopid.md §10). Corrupt/missing file = empty
 * tables + a warning: never a boot failure.
 */
#include "autopid_private.h"

#include "autopid_bus.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "expression_parser.h"
#include "obd_chip_guard.h" /* pure: shared with the host test build */

static void cfg_err(char *err, size_t err_len, const char *fmt,
                    const char *a, int b)
{
    if (err != NULL && err_len > 0)
    {
        snprintf(err, err_len, fmt, a, b);
    }
}

static void copy_str(char *dst, size_t cap, const cJSON *obj,
                     const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    dst[0] = '\0';

    if (cJSON_IsString(v) && v->valuestring != NULL)
    {
        snprintf(dst, cap, "%s", v->valuestring);
    }
}

static double get_num(const cJSON *obj, const char *key, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool get_bool(const cJSON *obj, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

void ap_init_sanitize(char *str)
{
    /* the chip driver's pure EEPROM guard (obd_chip_guard.h, 2026-09-16)
       does the ATSP->ATTP / ATM1->ATM0 rewrite for every requester; the
       refused set (ATPP/ATSD/ATCV/STWBR) is rejected at config parse
       (below) and by the driver at send time */
    if (str != NULL)
    {
        (void)obd_chip_guard_cmd(str, strlen(str));

        /* and the print-format commands the legacy firmware dropped from
           every init (ATH0 / ATS0 / ATE1): the poller reads the chip with
           headers and spaces on (2026-10-24, issue #1014) */
        (void)ap_chain_strip_print(str);

        /* and the words the chip stores every time it takes them (a
           protocol preset, a link word; autopid_bus.h, 2026-11-11): the
           tables refuse them where they are saved, this is the last stop
           before the wire for whatever was stored earlier */
        (void)ap_chain_strip_store(str);
    }
}

static int find_group(const ap_config_t *cfg, const char *name)
{
    for (int g = 0; g < cfg->n_groups; g++)
    {
        if (strcmp(cfg->groups[g].name, name) == 0)
        {
            return g;
        }
    }

    return -1;
}

/* Parse one parameters[] array into the shared pool slice. */
static bool parse_params(ap_config_t *cfg, const cJSON *arr,
                         uint16_t *start, uint16_t *count,
                         const char *owner, char *err, size_t err_len)
{
    *start = cfg->n_params;
    *count = 0;

    const cJSON *p = NULL;

    cJSON_ArrayForEach(p, arr)
    {
        if (*count >= AP_PARAMS_PER)
        {
            cfg_err(err, err_len, "%s: more than %d parameters", owner,
                    AP_PARAMS_PER);
            return false;
        }

        if (cfg->n_params >= AP_MAX_PARAMS)
        {
            cfg_err(err, err_len, "parameter pool full (%s%d)", "",
                    AP_MAX_PARAMS);
            return false;
        }

        ap_param_t *out = &cfg->params[cfg->n_params];

        copy_str(out->name, sizeof(out->name), p, "name");
        copy_str(out->expression, sizeof(out->expression), p, "expression");
        copy_str(out->mux_expr, sizeof(out->mux_expr), p, "mux_expr");
        copy_str(out->unit, sizeof(out->unit), p, "unit");
        copy_str(out->class, sizeof(out->class), p, "class");
        out->enabled = get_bool(p, "enabled", true);
        out->min = (float)get_num(p, "min", NAN);
        out->max = (float)get_num(p, "max", NAN);
        out->mux_val = (float)get_num(p, "mux_val", 0);

        if (out->name[0] == '\0')
        {
            cfg_err(err, err_len, "%s: parameter %d has no name", owner,
                    *count);
            return false;
        }

        char experr[64];

        if (out->expression[0] == '\0' ||
            expression_parser_check(out->expression, NULL, experr,
                                    sizeof(experr)) != ESP_OK)
        {
            cfg_err(err, err_len, "%s: bad expression (param %d)", owner,
                    *count);
            return false;
        }

        if (out->mux_expr[0] != '\0' &&
            expression_parser_check(out->mux_expr, NULL, experr,
                                    sizeof(experr)) != ESP_OK)
        {
            cfg_err(err, err_len, "%s: bad mux expression (param %d)",
                    owner, *count);
            return false;
        }

        cfg->n_params++;
        (*count)++;
    }

    return true;
}

esp_err_t ap_config_parse(const char *json, ap_config_t *cfg, char *err,
                          size_t err_len)
{
    memset(cfg, 0, sizeof(*cfg));

    cJSON *root = cJSON_Parse(json);

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        cfg_err(err, err_len, "not a JSON object%s%.0d", "", 0);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = ESP_ERR_INVALID_ARG;

    /* ---- groups (a "default" group exists even when absent) ------------ */
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "groups");
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, arr)
    {
        if (cfg->n_groups >= AP_MAX_GROUPS)
        {
            cfg_err(err, err_len, "more than %s%d groups", "",
                    AP_MAX_GROUPS);
            goto out;
        }

        ap_group_t *g = &cfg->groups[cfg->n_groups];

        copy_str(g->name, sizeof(g->name), item, "name");
        g->enabled_default = get_bool(item, "enabled", true);
        g->period_ms = (uint32_t)get_num(item, "period_ms", 1000);

        if (g->name[0] == '\0' || find_group(cfg, g->name) >= 0)
        {
            /* find_group sees only rows already committed */
            cfg_err(err, err_len, "group %s%d: empty or duplicate name",
                    "", cfg->n_groups);
            goto out;
        }

        cfg->n_groups++;
    }

    if (find_group(cfg, "default") < 0)
    {
        if (cfg->n_groups >= AP_MAX_GROUPS)
        {
            cfg_err(err, err_len, "more than %s%d groups", "",
                    AP_MAX_GROUPS);
            goto out;
        }

        ap_group_t *g = &cfg->groups[cfg->n_groups++];

        snprintf(g->name, sizeof(g->name), "default");
        g->enabled_default = true;
        g->period_ms = 1000;
    }

    /* ---- pids ----------------------------------------------------------- */
    arr = cJSON_GetObjectItemCaseSensitive(root, "pids");

    cJSON_ArrayForEach(item, arr)
    {
        if (cfg->n_pids >= AP_MAX_PIDS)
        {
            cfg_err(err, err_len, "more than %s%d pids", "", AP_MAX_PIDS);
            goto out;
        }

        ap_pid_t *pid = &cfg->pids[cfg->n_pids];
        char group[AP_NAME_LEN], type[16];

        copy_str(pid->name, sizeof(pid->name), item, "name");
        copy_str(pid->cmd, sizeof(pid->cmd), item, "cmd");
        copy_str(pid->init, sizeof(pid->init), item, "init");
        copy_str(pid->rxheader, sizeof(pid->rxheader), item, "rxheader");

        /* a J1939 parameter group: "PGN:<hex>[@<source>][?]" (autopid_j1939.h),
           served from the listener's store: nothing of the chip applies */
        esp_err_t pgn_rc = ap_pgn_cmd_parse(pid->cmd, &pid->pgn,
                                            &pid->j1939_sa,
                                            &pid->j1939_request);

        if (pgn_rc == ESP_OK)
        {
            pid->j1939 = true;

            if (pid->init[0] != '\0' || pid->rxheader[0] != '\0')
            {
                cfg_err(err, err_len, "pid %s%d: init and rxheader do not "
                        "apply to a PGN row", "", cfg->n_pids);
                goto out;
            }
        }
        else if (pgn_rc != ESP_ERR_NOT_FOUND)
        {
            cfg_err(err, err_len, "pid %s%d: bad PGN command, expected "
                    "PGN:<hex>[@<source>][?]", "", cfg->n_pids);
            goto out;
        }

        /* a word the chip stores every time it takes it (a protocol preset,
           a link word; autopid_bus.h), looked for before the sanitizing
           drops it. A row that is switched on is refused further down; one
           that is switched off keeps it in the file for the user to fix */
        char stored[16] = "";
        bool cmd_given = (pid->cmd[0] != '\0');
        bool stores = ap_chain_store_word(pid->cmd, stored, sizeof(stored)) ||
                      ap_chain_store_word(pid->init, stored, sizeof(stored));

        ap_init_sanitize(pid->cmd); /* profile/custom PIDs can carry AT
                                       commands: spare the EEPROM here
                                       too, not just in init strings */
        ap_init_sanitize(pid->init);

        if (obd_chip_guard_check(pid->cmd, strlen(pid->cmd)) ==
                OBD_GUARD_BLOCKED ||
            obd_chip_guard_check(pid->init, strlen(pid->init)) ==
                OBD_GUARD_BLOCKED)
        {
            cfg_err(err, err_len, "pid %s%d: cmd/init would write the "
                    "chip's EEPROM (ATPP/ATSD/ATCV/STWBR)", "", cfg->n_pids);
            goto out;
        }

        /* the row's bus (autopid_bus.h): the car's OBD protocol, or one of
           the buses only the chip reaches. The field is the whole choice: a
           row of another bus that picks a protocol itself is refused */
        char bus_word[8] = "";
        ap_row_bus_t bus = AP_BUS_OBD;

        copy_str(bus_word, sizeof(bus_word), item, "bus");

        if (!ap_bus_parse(bus_word, &bus))
        {
            cfg_err(err, err_len, "pid %s%d: unknown bus (obd, ms, sw or ch)",
                    "", cfg->n_pids);
            goto out;
        }

        /* and its speed, for a car whose bus does not run at the usual one */
        if (!ap_bus_rate_parse((uint8_t)bus, get_num(item, "bus_kbps", 0),
                               &pid->bus_rate))
        {
            cfg_err(err, err_len, "pid %s%d: bus_kbps needs a bus other than "
                    "obd and a speed the chip can make (500 over 1 to 64, "
                    "or that times 8/7)", "", cfg->n_pids);
            goto out;
        }

        if (bus != AP_BUS_OBD &&
            (pid->j1939 || !ap_bus_chain_allowed(pid->init) ||
             !ap_bus_chain_allowed(pid->cmd)))
        {
            cfg_err(err, err_len, "pid %s%d: a row on another bus cannot be "
                    "a PGN row, and its init and cmd must not pick a "
                    "protocol or reset the chip", "", cfg->n_pids);
            goto out;
        }

        pid->bus = (uint8_t)bus;
        copy_str(group, sizeof(group), item, "group");
        copy_str(type, sizeof(type), item, "type");
        pid->period_ms = (uint32_t)get_num(item, "period_ms", 0);
        pid->enabled = get_bool(item, "enabled", true);

        pid->type = (strcmp(type, "std") == 0)      ? AP_PID_STD
                    : (strcmp(type, "specific") == 0) ? AP_PID_SPECIFIC
                                                      : AP_PID_CUSTOM;

        if (pid->name[0] == '\0' || !cmd_given)
        {
            cfg_err(err, err_len, "pid %s%d: name and cmd are required",
                    "", cfg->n_pids);
            goto out;
        }

        if (stores && pid->enabled)
        {
            char who[AP_NAME_LEN + sizeof(stored) + 4];

            snprintf(who, sizeof(who), "%s: %s", pid->name, stored);
            cfg_err(err, err_len, "pid %s is stored by the OBD chip; use "
                    "the row's Bus%.0d", who, 0);
            goto out;
        }

        pid->group = find_group(cfg, group[0] ? group : "default");

        if (pid->group < 0)
        {
            cfg_err(err, err_len, "pid %s: unknown group%.0d", pid->name, 0);
            goto out;
        }

        if (!parse_params(cfg,
                          cJSON_GetObjectItemCaseSensitive(item,
                                                           "parameters"),
                          &pid->param_start, &pid->param_count, pid->name,
                          err, err_len))
        {
            goto out;
        }

        cfg->n_pids++;
    }

    /* ---- filters --------------------------------------------------------- */
    arr = cJSON_GetObjectItemCaseSensitive(root, "filters");

    cJSON_ArrayForEach(item, arr)
    {
        if (cfg->n_filters >= AP_MAX_FILTERS)
        {
            cfg_err(err, err_len, "more than %s%d filters", "",
                    AP_MAX_FILTERS);
            goto out;
        }

        ap_filter_t *f = &cfg->filters[cfg->n_filters];
        char group[AP_NAME_LEN];
        char owner[AP_NAME_LEN + 8];

        f->frame_id = (uint32_t)get_num(item, "frame_id", 0);
        f->is_extended = f->frame_id > 0x7FF;
        f->monitor_ms = (uint32_t)get_num(item, "monitor_ms", 1000);
        f->period_ms = (uint32_t)get_num(item, "period_ms", 0);
        f->enabled = get_bool(item, "enabled", true);
        copy_str(group, sizeof(group), item, "group");
        snprintf(owner, sizeof(owner), "filter %X", (unsigned)f->frame_id);

        if (f->frame_id == 0)
        {
            cfg_err(err, err_len, "filter %s%d: frame_id required", "",
                    cfg->n_filters);
            goto out;
        }

        if (f->monitor_ms < 50 || f->monitor_ms > 60000)
        {
            /* the window HOLDS the chip (MONITOR claim): keep it sane */
            cfg_err(err, err_len, "%s: monitor_ms 50..60000%.0d", owner,
                    0);
            goto out;
        }

        f->group = find_group(cfg, group[0] ? group : "default");

        if (f->group < 0)
        {
            cfg_err(err, err_len, "filter %s: unknown group%.0d", owner, 0);
            goto out;
        }

        if (!parse_params(cfg,
                          cJSON_GetObjectItemCaseSensitive(item,
                                                           "parameters"),
                          &f->param_start, &f->param_count, owner, err,
                          err_len))
        {
            goto out;
        }

        cfg->n_filters++;
    }

    /* duplicate parameter names break the cache/API keying */
    for (int a = 0; a < cfg->n_params; a++)
    {
        for (int b = a + 1; b < cfg->n_params; b++)
        {
            if (strcmp(cfg->params[a].name, cfg->params[b].name) == 0)
            {
                cfg_err(err, err_len, "duplicate parameter name '%s'%.0d",
                        cfg->params[a].name, 0);
                goto out;
            }
        }
    }

    rc = ESP_OK;

out:
    if (rc != ESP_OK)
    {
        memset(cfg, 0, sizeof(*cfg));
    }

    cJSON_Delete(root);
    return rc;
}
