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
 * @file autopid_dtc_report.c
 * @brief DTC report as JSON (the GET /api/autopid/dtc "report" object)
 *        and the public autopid_dtc_* wrappers the script engine binds.
 *        Split out of autopid_dtc.c 2026-10-02 (700-line rule).
 *
 * The report is read IN PLACE under the engine's lock (2026-10-03): it
 * grew to about 5 KB with the per-ECU items, and until then a copy of it
 * went onto the caller's stack, twice in the HTTP handler.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "autopid.h"
#include "autopid_private.h"
#include "autopid_dtc_engine.h"

static void add_code_array(cJSON *o, const char *key,
                           const char codes[][AP_DTC_CODE_LEN], uint8_t n)
{
    cJSON *arr = cJSON_AddArrayToObject(o, key);

    for (uint8_t i = 0; i < n && arr != NULL; i++)
    {
        cJSON_AddItemToArray(arr, cJSON_CreateString(codes[i]));
    }
}

/** A responder's CAN id as text ("7E8", "18DAF100"). */
static void ecu_text(uint32_t ecu, char *out, size_t cap)
{
    snprintf(out, cap, "%lX", (unsigned long)ecu);
}

/** `lamps`: the four J1939 lamps of a DM1 (SAE J1939-73). */
static void add_lamps(cJSON *o, uint8_t lamps)
{
    cJSON *l = cJSON_AddObjectToObject(o, "lamps");

    if (l != NULL)
    {
        cJSON_AddBoolToObject(l, "mil", (lamps & AP_DTC_LAMP_MIL) != 0);
        cJSON_AddBoolToObject(l, "rsl", (lamps & AP_DTC_LAMP_RSL) != 0);
        cJSON_AddBoolToObject(l, "awl", (lamps & AP_DTC_LAMP_AWL) != 0);
        cJSON_AddBoolToObject(l, "pl", (lamps & AP_DTC_LAMP_PL) != 0);
    }
}

/* ---- public wrappers (the script_engine binding surface) ------------------ */

esp_err_t autopid_dtc_scan_start(void)
{
    return ap_dtc_scan_start();
}

bool autopid_dtc_scanning(void)
{
    return ap_dtc_busy();
}

esp_err_t autopid_dtc_report(cJSON **out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = ap_dtc_report_json();
    return (*out != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t autopid_dtc_clear(const char *codes, const char *mode,
                            bool *out_cleared)
{
    char err[48];

    return ap_dtc_clear(codes, mode, out_cleared, NULL, NULL, err,
                        sizeof(err));
}

/* ---- the JSON view ---------------------------------------------------------- */

/** `sources`: every ECU that answered the lamp request, with its word. */
static void add_sources(cJSON *o, const ap_dtc_report_t *r)
{
    cJSON *arr = cJSON_AddArrayToObject(o, "sources");

    for (uint8_t i = 0; i < r->n_src && arr != NULL; i++)
    {
        cJSON *s = cJSON_CreateObject();

        if (s == NULL)
        {
            break;
        }

        if (r->src[i].j1939)
        {
            /* a J1939 controller: its source address and its lamps */
            cJSON_AddNumberToObject(s, "sa", r->src[i].ecu);
            add_lamps(s, r->src[i].lamps);
        }
        else if (r->src[i].ecu != UINT32_MAX)
        {
            char hdr[10];

            ecu_text(r->src[i].ecu, hdr, sizeof(hdr));
            cJSON_AddStringToObject(s, "ecu", hdr);
        }

        cJSON_AddBoolToObject(s, "mil", r->src[i].mil);
        cJSON_AddNumberToObject(s, "count", r->src[i].count);
        cJSON_AddItemToArray(arr, s);
    }
}

/** `items`: who reported what (code, category, ECU; the status and
 *  severity bytes where the service carries them). */
static void add_items(cJSON *o, const ap_dtc_report_t *r)
{
    cJSON *arr = cJSON_AddArrayToObject(o, "items");

    for (uint8_t i = 0; i < r->n_items && arr != NULL; i++)
    {
        const ap_dtc_item_t *it = &r->items[i];
        cJSON *e = cJSON_CreateObject();

        if (e == NULL)
        {
            break;
        }

        cJSON_AddStringToObject(e, "code", it->code);
        cJSON_AddStringToObject(e, "kind",
                                ap_dtc_kind_name((ap_dtc_kind_t)it->kind));

        if (it->j1939)
        {
            /* a DM1 code: who sent it, how often it occurred (127 = not
               available) */
            cJSON_AddNumberToObject(e, "sa", it->ecu);

            if (it->oc != 127)
            {
                cJSON_AddNumberToObject(e, "oc", it->oc);
            }
        }
        else if (it->ecu != UINT32_MAX)
        {
            char hdr[10];

            ecu_text(it->ecu, hdr, sizeof(hdr));
            cJSON_AddStringToObject(e, "ecu", hdr);
        }

        if (it->status != 0)
        {
            cJSON_AddNumberToObject(e, "status", it->status);
        }

        if (it->severity != 0)
        {
            cJSON_AddNumberToObject(e, "severity", it->severity);
        }

        cJSON_AddItemToArray(arr, e);
    }
}

/** The freeze frame (§14): absent when none was captured. */
static void add_freeze(cJSON *o, const ap_dtc_report_t *r)
{
    if (!r->frz_present)
    {
        return;
    }

    cJSON *fz = cJSON_AddObjectToObject(o, "freeze");

    if (fz == NULL)
    {
        return;
    }

    cJSON_AddStringToObject(fz, "dtc", r->frz_dtc);

    if (r->frz_ecu != UINT32_MAX)
    {
        char hdr[10];

        ecu_text(r->frz_ecu, hdr, sizeof(hdr));
        cJSON_AddStringToObject(fz, "ecu", hdr);
    }

    cJSON *pp = cJSON_AddObjectToObject(fz, "params");

    for (uint8_t i = 0; i < r->n_frz && pp != NULL; i++)
    {
        cJSON *v = cJSON_AddObjectToObject(pp, r->frz[i].name);

        if (v != NULL)
        {
            cJSON_AddNumberToObject(v, "value", (double)r->frz[i].value);
            cJSON_AddStringToObject(v, "unit", r->frz[i].unit);
        }
    }
}

static cJSON *report_json(bool with_desc)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    const ap_dtc_report_t *r = ap_dtc_report_lock();

    cJSON_AddBoolToObject(o, "valid", r->valid);
    cJSON_AddNumberToObject(o, "ts", (double)r->ts_epoch);
    cJSON_AddBoolToObject(o, "mil", r->mil);
    cJSON_AddNumberToObject(o, "mil_count", r->mil_count);
    cJSON_AddNumberToObject(o, "ecus", r->n_ecus);
    cJSON_AddStringToObject(o, "protocol",
                            r->protocol[0] ? r->protocol : "obd");
    cJSON_AddBoolToObject(o, "j1939", r->j1939);

    if (r->j1939)
    {
        add_lamps(o, r->lamps);
    }

    add_code_array(o, "stored", r->stored, r->n_stored);
    add_code_array(o, "pending", r->pending, r->n_pending);
    add_code_array(o, "permanent", r->permanent, r->n_permanent);
    add_code_array(o, "new", r->new_codes, r->n_new);
    add_sources(o, r);
    add_items(o, r);
    add_freeze(o, r);

    if (r->error[0] != '\0')
    {
        cJSON_AddStringToObject(o, "error", r->error);
    }

    /* database enrichment: {"desc":{"P0420":"Catalyst …"}} for every
       report code with a hit (arrays stay untouched, no breakage) */
    if (with_desc && r->valid)
    {
        ap_dtc_db_desc_map(o, "desc", r);
    }

    ap_dtc_report_unlock();
    return o;
}

cJSON *ap_dtc_report_json(void)
{
    return report_json(false);
}

cJSON *ap_dtc_report_json_desc(void)
{
    return report_json(true);
}
