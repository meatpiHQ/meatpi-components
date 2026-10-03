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
 * @file autopid_j1939_std.c
 * @brief The J1939 standard set (TASK_j1939_wwh.md phase 5): the built-in
 *        SPN table of the j1939 component as config rows, and the
 *        listener's view of the vehicle for the detection job and first
 *        contact. Target half of autopid_j1939.h; the grammar and the
 *        expressions are pure (autopid_j1939_core.c).
 *
 * One row per SPN, like one row per PID on the OBD side: `PGN:F004` /
 * EngineSpeed, `PGN:F004` / ActualEnginePercentTorque. The runner reads the
 * store, so three rows of one group cost three lookups, not three requests.
 * No source is pinned: the listener's pick (the lowest address heard in 5 s)
 * is the engine on a tractor; a user pins `@<sa>` by hand when two units
 * send the same group.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "can_manager.h"
#include "j1939.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

#define AP_J1939_VIN_ASK_US  30000000 /* ask for the VIN once per half minute */
#define AP_J1939_VIN_WAIT_MS 1500     /* and give the answer this long        */

static int64_t s_vin_asked_us = -AP_J1939_VIN_ASK_US; /* the first ask: now */

bool ap_j1939_std_entry_to_json(cJSON *arr, const j1939_spn_t *spn, int sa)
{
    char cmd[AP_CMD_LEN];
    char expr[AP_EXPR_LEN];

    if (arr == NULL || spn == NULL ||
        ap_pgn_cmd_format(spn->pgn, sa, false, cmd, sizeof(cmd)) == 0 ||
        ap_j1939_std_expression(spn, expr, sizeof(expr)) == 0)
    {
        return false;
    }

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return false;
    }

    char pgn_hex[8];

    snprintf(pgn_hex, sizeof(pgn_hex), "%lX", (unsigned long)spn->pgn);
    cJSON_AddStringToObject(o, "pgn", pgn_hex);
    cJSON_AddNumberToObject(o, "spn", spn->spn);
    cJSON_AddStringToObject(o, "cmd", cmd);
    cJSON_AddStringToObject(o, "name", spn->name);

    cJSON *params = cJSON_AddArrayToObject(o, "parameters");
    cJSON *pj = cJSON_CreateObject();

    if (params == NULL || pj == NULL)
    {
        cJSON_Delete(pj);
        cJSON_Delete(o);
        return false;
    }

    cJSON_AddStringToObject(pj, "name", spn->name);
    cJSON_AddStringToObject(pj, "expression", expr);
    cJSON_AddStringToObject(pj, "unit", spn->unit ? spn->unit : "");
    cJSON_AddStringToObject(pj, "class", spn->dev_class ? spn->dev_class : "");
    /* the operational range: "not available" (FF..) and error (FE..) raw
       values decode beyond max, and the plausibility clamp drops them */
    cJSON_AddNumberToObject(pj, "min", spn->min);
    cJSON_AddNumberToObject(pj, "max", spn->max);
    cJSON_AddItemToArray(params, pj);
    cJSON_AddItemToArray(arr, o);
    return true;
}

size_t ap_j1939_std_table_json(cJSON *arr)
{
    size_t count = 0;
    const j1939_spn_t *tab = j1939_spn_table(&count);
    size_t n = 0;

    for (size_t i = 0; tab != NULL && i < count; i++)
    {
        if (ap_j1939_std_entry_to_json(arr, &tab[i], -1))
        {
            n++;
        }
    }

    return n;
}

bool ap_j1939_listening(void)
{
    j1939_status_t st;

    return j1939_status(&st) == ESP_OK && st.state == J1939_STATE_LISTENING;
}

bool ap_j1939_identify(ap_veh_seen_t *seen, cJSON *supported,
                       uint16_t *found)
{
    j1939_status_t st;

    if (seen == NULL || j1939_status(&st) != ESP_OK ||
        st.state != J1939_STATE_LISTENING || st.bus != J1939_BUS_J1939)
    {
        return false;
    }

    seen->j1939 = true;

    /* the responder set: the source addresses heard, lowest first (the
       null address 254 belongs to nobody); the chip's responders, when the
       vehicle has an OBD dialect too, are the better fingerprint and stay */
    if (seen->n_ecus == 0)
    {
        j1939_source_t src[AP_VEH_ECUS_MAX + 2];
        size_t n = j1939_sources(src, AP_VEH_ECUS_MAX + 2);

        for (size_t i = 0; i < n && seen->n_ecus < AP_VEH_ECUS_MAX; i++)
        {
            if (src[i].sa >= J1939_ADDR_NULL)
            {
                continue;
            }

            seen->ecus[seen->n_ecus].id = src[i].sa;
            seen->ecus[seen->n_ecus].bitmap = 0; /* a NAME is claimed on
                                                    some boots only: not a
                                                    stable print */
            seen->n_ecus++;
        }
    }

    if (seen->vin[0] == '\0')
    {
        char vin[J1939_VIN_LEN + 1];
        uint8_t sa = 0;
        bool have = j1939_vin(vin, &sa);

        /* active mode: the VIN is an on-request group on most vehicles; ask
           once per look-round and give the answer (a BAM, three packets
           50 ms apart) a moment. Blocks the caller: the scan job, or the
           poller while a J1939 car is being identified */
        if (!have && j1939_active() &&
            esp_timer_get_time() - s_vin_asked_us > AP_J1939_VIN_ASK_US &&
            j1939_request(J1939_PGN_VIN, J1939_ADDR_GLOBAL) == ESP_OK)
        {
            s_vin_asked_us = esp_timer_get_time();

            for (int i = 0; i < AP_J1939_VIN_WAIT_MS / 50 && !have; i++)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                have = j1939_vin(vin, &sa);
            }

            ESP_LOGI(TAG, "detection: VIN asked for on the J1939 network, %s",
                     have ? "answered" : "no answer yet");
        }

        if (have && ap_veh_vin_valid(vin))
        {
            snprintf(seen->vin, sizeof(seen->vin), "%s", vin);
        }
    }

    /* the built-in rows whose group somebody sends; a value that is "not
       available" right now (engine off) still names a row */
    if (supported != NULL)
    {
        size_t count = 0;
        const j1939_spn_t *tab = j1939_spn_table(&count);

        for (size_t i = 0; tab != NULL && i < count; i++)
        {
            j1939_msg_t info;

            if (j1939_pgn_latest(tab[i].pgn, J1939_ADDR_ANY, J1939_ADDR_ANY,
                                 &info, NULL, 0) == ESP_OK &&
                ap_j1939_std_entry_to_json(supported, &tab[i], -1) &&
                found != NULL)
            {
                (*found)++;
            }
        }
    }

    return true;
}

/* ---- without the listener: a sample of the bus -------------------------------- */

#define AP_J1939_SAMPLE_MS  1000  /* every well-known group is sent at least
                                     once a second                           */
#define AP_J1939_SAMPLE_IDS 48

/** The rows of the groups in @p ids, appended to @p supported. */
static void rows_of_sample(const can_id_seen_t *ids, size_t n,
                           cJSON *supported, uint16_t *found)
{
    size_t count = 0;
    const j1939_spn_t *tab = j1939_spn_table(&count);

    for (size_t i = 0; tab != NULL && i < count; i++)
    {
        bool seen = false;

        for (size_t k = 0; k < n && !seen; k++)
        {
            j1939_id_t id;

            if (!ids[k].ext)
            {
                continue;
            }

            j1939_id_parse(ids[k].id, &id);
            seen = (id.pgn == tab[i].pgn);
        }

        if (seen && ap_j1939_std_entry_to_json(supported, &tab[i], -1) &&
            found != NULL)
        {
            (*found)++;
        }
    }
}

/** One source address into the responder set (the eight lowest stay). */
static void note_source(ap_veh_seen_t *seen, uint8_t sa)
{
    for (size_t k = 0; k < seen->n_ecus; k++)
    {
        if (seen->ecus[k].id == sa)
        {
            return;
        }
    }

    if (seen->n_ecus < AP_VEH_ECUS_MAX)
    {
        seen->ecus[seen->n_ecus].id = sa;
        seen->ecus[seen->n_ecus].bitmap = 0;
        seen->n_ecus++;
        return;
    }

    size_t hi = 0;

    for (size_t m = 1; m < seen->n_ecus; m++)
    {
        if (seen->ecus[m].id > seen->ecus[hi].id)
        {
            hi = m;
        }
    }

    if (seen->ecus[hi].id > sa)
    {
        seen->ecus[hi].id = sa;
    }
}

bool ap_j1939_identify_sample(ap_veh_seen_t *seen, cJSON *supported,
                              uint16_t *found)
{
    static can_id_seen_t ids[AP_J1939_SAMPLE_IDS] EXT_RAM_BSS_ATTR; /* scan
                                                                  task only */
    size_t n = 0;
    uint32_t frames = 0;

    if (seen == NULL)
    {
        return false;
    }

    esp_err_t err = can_manager_sample_ids(AP_J1939_SAMPLE_MS, ids,
                                           AP_J1939_SAMPLE_IDS, &n, &frames);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "detection: the bus could not be sampled (%s)",
                 esp_err_to_name(err));
        return false;
    }

    /* the j1939 component's own rule: well-known groups make a J1939 bus */
    j1939_bus_evidence_t ev;

    memset(&ev, 0, sizeof(ev));

    for (size_t i = 0; i < n; i++)
    {
        j1939_bus_note(&ev, ids[i].id, ids[i].ext, false, ids[i].dlc);
    }

    ESP_LOGI(TAG, "detection: bus sample %lu frames, %u ids: %s",
             (unsigned long)frames, (unsigned)n,
             j1939_bus_name(j1939_bus_verdict(&ev)));

    if (j1939_bus_verdict(&ev) != J1939_BUS_J1939)
    {
        return false;
    }

    seen->j1939 = true;

    if (seen->n_ecus == 0)
    {
        /* the source addresses as the responder set (the chip's responders,
           when the vehicle has an OBD dialect too, are the better print) */
        for (size_t i = 0; i < n; i++)
        {
            j1939_id_t id;

            if (!ids[i].ext)
            {
                continue;
            }

            j1939_id_parse(ids[i].id, &id);

            if (id.sa < J1939_ADDR_NULL)
            {
                note_source(seen, id.sa);
            }
        }
    }

    if (supported != NULL)
    {
        rows_of_sample(ids, n, supported, found);
    }

    return true;
}
