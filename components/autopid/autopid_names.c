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
 * @file autopid_names.c
 * @brief Parameter names made unique, PURE (2026-10-06). Parameter names
 *        key the value cache and the API, so the config parser refuses a
 *        name that appears twice. The SAE table used to give the same name
 *        to two PIDs (OxySensor1_Volt under 0x14 and 0x24, 21 such pairs)
 *        and the detection wrote those rows unvalidated: a car answering
 *        both sets got a file its own loader then refused at boot ("empty
 *        tables", a boot_errors fault) and the wizard's PUT of it was
 *        refused too (Ali, 2026-10-06). The table is fixed; this is the
 *        guard that holds whatever the table says: the scan's rows are
 *        made unique before they are stored, and a stored file with a
 *        repeat is repaired at load instead of emptied. Host-tested.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "autopid_private.h"

/* is @p name already the name of a parameter of an earlier row, or of an
   earlier parameter of the same row? */
static bool name_taken(const cJSON *pids, const cJSON *upto_pid,
                       const cJSON *upto_param, const char *name)
{
    const cJSON *pid = NULL;

    cJSON_ArrayForEach(pid, pids)
    {
        const cJSON *prm = NULL;

        cJSON_ArrayForEach(prm, cJSON_GetObjectItemCaseSensitive(pid,
                                                                 "parameters"))
        {
            if (pid == upto_pid && prm == upto_param)
            {
                return false; /* reached ourselves: nothing before us */
            }

            const cJSON *n = cJSON_GetObjectItemCaseSensitive(prm, "name");

            if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            {
                return true;
            }
        }

        if (pid == upto_pid)
        {
            break;
        }
    }

    return false;
}

int ap_names_dedupe(cJSON *pids)
{
    int renamed = 0;
    cJSON *pid = NULL;

    cJSON_ArrayForEach(pid, pids)
    {
        cJSON *prm = NULL;

        cJSON_ArrayForEach(prm, cJSON_GetObjectItemCaseSensitive(pid,
                                                                 "parameters"))
        {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(prm, "name");

            if (!cJSON_IsString(n) || n->valuestring[0] == '\0' ||
                !name_taken(pids, pid, prm, n->valuestring))
            {
                continue;
            }

            /* the first free suffix: name_2, name_3, ... (the profile
               importer in the web UI renames the same way) */
            char fresh[AP_NAME_LEN];

            for (int k = 2; k < 100; k++)
            {
                snprintf(fresh, sizeof(fresh), "%.*s_%d",
                         (int)(sizeof(fresh) - 5), n->valuestring, k);

                if (!name_taken(pids, pid, prm, fresh))
                {
                    break;
                }
            }

            if (cJSON_SetValuestring(n, fresh) != NULL)
            {
                renamed++;
            }
        }
    }

    return renamed;
}

char *ap_config_repair_names(const char *json, int *renamed)
{
    cJSON *root = cJSON_Parse(json);
    char *out = NULL;

    *renamed = 0;

    if (root == NULL)
    {
        return NULL; /* not ours to judge: the parser says what is wrong */
    }

    *renamed = ap_names_dedupe(cJSON_GetObjectItemCaseSensitive(root,
                                                                "pids"));

    if (*renamed > 0)
    {
        out = cJSON_PrintUnformatted(root);
    }

    cJSON_Delete(root);
    return out;
}
