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
 * @file autopid_vehicle_codec.c
 * @brief PURE vehicle store codec (TASK_quick_setup.md, second pass):
 *        the responder table as text, one entry as its JSON object (the
 *        API and file shape) and the vehicles.json round trip with its
 *        bounds (at most AP_VEH_MAX entries, keys validated or derived,
 *        VIN / protocol / fingerprint fields sanitized). Split from
 *        autopid_vehicle_index.c to keep both under the standard's 700
 *        lines. No IDF dependencies; host-tested
 *        (host_test/main/test_vehicle_index.c).
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static bool fp_valid(const char *fp)
{
    if (fp == NULL || strlen(fp) != AP_FP_LEN - 1)
    {
        return false;
    }

    for (int i = 0; i < AP_FP_LEN - 1; i++)
    {
        char c = fp[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }

    return true;
}

/* ---- the responder table as text ---------------------------------------------------- */

int ap_veh_ecus_to_str(const ap_veh_ecu_t *ecus, size_t n, char *out,
                       size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return -1;
    }

    out[0] = '\0';

    size_t w = 0;
    int count = 0;

    for (size_t i = 0; ecus != NULL && i < n; i++)
    {
        char tok[24];
        int len;

        if (ecus[i].id == UINT32_MAX)
        {
            len = snprintf(tok, sizeof(tok), "%s*:%08X", (i > 0) ? "," : "",
                           (unsigned)ecus[i].bitmap);
        }
        else
        {
            len = snprintf(tok, sizeof(tok), "%s%X:%08X", (i > 0) ? "," : "",
                           (unsigned)ecus[i].id, (unsigned)ecus[i].bitmap);
        }

        if (len < 0 || w + (size_t)len >= cap)
        {
            return -1;
        }

        memcpy(out + w, tok, (size_t)len + 1);
        w += (size_t)len;
        count++;
    }

    return count;
}

int ap_veh_ecus_from_str(const char *s, ap_veh_ecu_t *out, size_t max)
{
    int n = 0;

    if (s == NULL || out == NULL)
    {
        return 0;
    }

    while (*s != '\0' && (size_t)n < max)
    {
        const char *end = strchr(s, ',');
        size_t len = (end != NULL) ? (size_t)(end - s) : strlen(s);
        const char *colon = memchr(s, ':', len);

        if (colon != NULL && colon > s && (size_t)(colon - s) <= 8 &&
            len - (size_t)(colon - s) - 1 >= 1 &&
            len - (size_t)(colon - s) - 1 <= 8)
        {
            char idtok[9], bmtok[9];
            size_t il = (size_t)(colon - s);
            size_t bl = len - il - 1;
            bool ok = true;

            memcpy(idtok, s, il);
            idtok[il] = '\0';
            memcpy(bmtok, colon + 1, bl);
            bmtok[bl] = '\0';

            for (size_t i = 0; i < bl && ok; i++)
            {
                ok = isxdigit((unsigned char)bmtok[i]) != 0;
            }

            bool star = (strcmp(idtok, "*") == 0);

            for (size_t i = 0; !star && i < il && ok; i++)
            {
                ok = isxdigit((unsigned char)idtok[i]) != 0;
            }

            if (ok)
            {
                out[n].id = star ? UINT32_MAX
                                 : (uint32_t)strtoul(idtok, NULL, 16);
                out[n].bitmap = (uint32_t)strtoul(bmtok, NULL, 16);
                n++;
            }
        }

        s += len + ((end != NULL) ? 1 : 0);
    }

    return n;
}

/* ---- vehicles.json ----------------------------------------------------------------- */

struct cJSON *ap_vidx_entry_json(const ap_veh_entry_t *e, bool with_current,
                                 bool current)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL || e == NULL)
    {
        cJSON_Delete(o);
        return NULL;
    }

    char ecus[AP_VEH_ECUS_STR_LEN];

    (void)ap_veh_ecus_to_str(e->ecus, e->n_ecus, ecus, sizeof(ecus));

    cJSON_AddStringToObject(o, "key", e->key);
    cJSON_AddStringToObject(o, "vin", e->vin);
    cJSON_AddStringToObject(o, "fingerprint", e->fingerprint);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "protocol", e->protocol);
    cJSON_AddStringToObject(o, "chip_protocol", e->chip_protocol);
    cJSON_AddStringToObject(o, "dialect",
                            ap_dialect_name((ap_dialect_t)e->dialect));
    cJSON_AddBoolToObject(o, "j1939", e->j1939);
    cJSON_AddStringToObject(o, "profile", e->profile);
    cJSON_AddStringToObject(o, "specific_init", e->specific_init);
    cJSON_AddStringToObject(o, "ecus", ecus);
    cJSON_AddNumberToObject(o, "std_supported", e->std_supported);
    cJSON_AddBoolToObject(o, "pending_profile", e->pending_profile);
    cJSON_AddNumberToObject(o, "first_seen", (double)e->first_seen);
    cJSON_AddNumberToObject(o, "last_seen", (double)e->last_seen);
    cJSON_AddNumberToObject(o, "scan_ts", (double)e->scan_ts);

    if (with_current)
    {
        cJSON_AddBoolToObject(o, "current", current);
    }

    return o;
}

int ap_vidx_to_json(const ap_veh_index_t *idx, char *out, size_t cap)
{
    if (idx == NULL || out == NULL || cap == 0)
    {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        return -1;
    }

    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "current",
                            (idx->current >= 0 && idx->current < idx->n)
                                ? idx->v[idx->current].key : "");

    cJSON *arr = cJSON_AddArrayToObject(root, "vehicles");

    for (int i = 0; arr != NULL && i < idx->n; i++)
    {
        cJSON *e = ap_vidx_entry_json(&idx->v[i], false, false);

        if (e != NULL)
        {
            cJSON_AddItemToArray(arr, e);
        }
    }

    out[0] = '\0';

    bool ok = cJSON_PrintPreallocated(root, out, (int)cap, false) != 0;

    cJSON_Delete(root);

    if (!ok)
    {
        out[0] = '\0';
        return -1;
    }

    return (int)strlen(out);
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

static int64_t get_ts(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(v) || v->valuedouble < 0)
    {
        return 0;
    }

    return (int64_t)v->valuedouble;
}

static void sanitize_proto(char *p)
{
    char c = (char)toupper((unsigned char)p[0]);

    if (p[0] == '\0' || p[1] != '\0' || !ap_veh_proto_valid(c))
    {
        p[0] = '\0';
    }
    else
    {
        p[0] = c;
    }
}

/** One stored entry -> @p e; false when it has no usable key. */
static bool entry_from_json(const cJSON *item, ap_veh_entry_t *e)
{
    char ecus[AP_VEH_ECUS_STR_LEN];
    char dialect[8];

    memset(e, 0, sizeof(*e));

    if (!cJSON_IsObject(item))
    {
        return false;
    }

    copy_str(e->key, sizeof(e->key), item, "key");
    copy_str(e->vin, sizeof(e->vin), item, "vin");
    copy_str(e->fingerprint, sizeof(e->fingerprint), item, "fingerprint");
    copy_str(e->name, sizeof(e->name), item, "name");
    copy_str(e->protocol, sizeof(e->protocol), item, "protocol");
    copy_str(e->chip_protocol, sizeof(e->chip_protocol), item,
             "chip_protocol");
    copy_str(e->profile, sizeof(e->profile), item, "profile");
    copy_str(e->specific_init, sizeof(e->specific_init), item,
             "specific_init");
    copy_str(ecus, sizeof(ecus), item, "ecus");
    copy_str(dialect, sizeof(dialect), item, "dialect");

    e->n_ecus = (uint8_t)ap_veh_ecus_from_str(ecus, e->ecus,
                                             AP_VEH_ECUS_MAX);
    /* no field (a store written before 2026-10-03) = an OBD-II car */
    e->dialect = (uint8_t)ap_dialect_from_name(dialect);
    /* the vehicle network is J1939 (a truck; alone, or beside its OBD
       dialect): the listener's rows apply. A J1939-only car implies it. */
    e->j1939 = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "j1939")) ||
               e->dialect == AP_DIALECT_J1939;

    /* a hand-edited file must not smuggle a bad VIN / protocol / print */
    if (!ap_veh_vin_valid(e->vin))
    {
        e->vin[0] = '\0';
    }

    if (e->n_ecus > 0)
    {
        ap_veh_fingerprint(e->ecus, e->n_ecus, e->fingerprint);
    }
    else if (!fp_valid(e->fingerprint))
    {
        e->fingerprint[0] = '\0';
    }

    sanitize_proto(e->protocol);
    sanitize_proto(e->chip_protocol);

    if (!ap_vidx_key_valid(e->key))
    {
        ap_vidx_key_for(e->vin, e->fingerprint, e->key);
    }

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "std_supported");

    e->std_supported = (cJSON_IsNumber(v) && v->valuedouble >= 0 &&
                        v->valuedouble <= 65535)
                           ? (uint16_t)v->valuedouble : 0;
    e->pending_profile = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(item, "pending_profile"));
    e->first_seen = get_ts(item, "first_seen");
    e->last_seen = get_ts(item, "last_seen");
    e->scan_ts = get_ts(item, "scan_ts");

    return e->key[0] != '\0';
}

bool ap_vidx_from_json(const char *json, ap_veh_index_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    ap_vidx_init(out);

    if (json == NULL)
    {
        return false;
    }

    cJSON *root = cJSON_Parse(json);

    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");

    if (!cJSON_IsNumber(ver) || ver->valueint != 1)
    {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "vehicles");
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, arr)
    {
        if (out->n >= AP_VEH_MAX)
        {
            break;              /* bounded: the tail is dropped          */
        }

        ap_veh_entry_t *e = &out->v[out->n];

        if (!entry_from_json(item, e) || ap_vidx_find_key(out, e->key) >= 0)
        {
            memset(e, 0, sizeof(*e));
            continue;           /* no key, or a duplicate                */
        }

        out->n++;
    }

    char cur[AP_VEH_KEY_LEN];

    copy_str(cur, sizeof(cur), root, "current");
    out->current = (int8_t)ap_vidx_find_key(out, cur);

    cJSON_Delete(root);
    return true;
}
