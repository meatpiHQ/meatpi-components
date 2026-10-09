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
 * @file autopid_scan_rows.c
 * @brief PURE: the detection job's rows as a new car's tables document
 *        (config.json shape: standard rows only, the `default` group).
 *
 * The rows are what the car answers, not what the user chose, so every
 * one of them is stored `"enabled": false` (2026-10-09, Ali: "it should
 * not start polling unless the user enables the standard PIDs and enables
 * the PIDs they want"). Nothing standard is read until the standard-PID
 * switch is on AND a row is ticked, in the Quick Setup's picker or under
 * Automate > Parameters. Host-tested (test_scan_rows.c).
 */
#include "autopid_private.h"   /* brings autopid_dialect.h and cJSON */

#include <stdbool.h>
#include <string.h>

char *ap_scan_rows_config(const cJSON *supported)
{
    cJSON *root = cJSON_CreateObject();
    char *body = NULL;

    if (root == NULL)
    {
        return NULL;
    }

    cJSON_AddArrayToObject(root, "groups");

    cJSON *pids = cJSON_AddArrayToObject(root, "pids");
    const cJSON *e = NULL;

    cJSON_ArrayForEach(e, supported)
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(e, "name");
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(e, "cmd");
        const cJSON *init = cJSON_GetObjectItemCaseSensitive(e, "init");
        const cJSON *prm = cJSON_GetObjectItemCaseSensitive(e, "parameters");
        cJSON *pid = cJSON_CreateObject();

        if (pids == NULL || pid == NULL || !cJSON_IsString(name) ||
            !cJSON_IsString(cmd))
        {
            cJSON_Delete(pid);
            continue;
        }

        cJSON_AddStringToObject(pid, "name", name->valuestring);
        cJSON_AddStringToObject(pid, "type", "std");
        cJSON_AddStringToObject(pid, "cmd", cmd->valuestring);

        if (cJSON_IsString(init) && init->valuestring[0] != '\0')
        {
            /* the row's ECU (a UDS-dialect car) */
            cJSON_AddStringToObject(pid, "init", init->valuestring);
        }

        cJSON_AddStringToObject(pid, "group", "default");
        /* stored, not read: the user ticks the ones they want */
        cJSON_AddBoolToObject(pid, "enabled", false);
        cJSON_AddItemToObject(pid, "parameters",
                              cJSON_IsArray(prm) ? cJSON_Duplicate(prm, true)
                                                 : cJSON_CreateArray());
        cJSON_AddItemToArray(pids, pid);
    }

    cJSON_AddArrayToObject(root, "filters");
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}
