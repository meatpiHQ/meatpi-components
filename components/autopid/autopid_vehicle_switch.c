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
 * @file autopid_vehicle_switch.c
 * @brief The vehicle store's file side (TASK_quick_setup.md, second
 *        pass): the per-car tables files under /data/autopid/vehicles/,
 *        the switch (snapshot the live config.json into the previous
 *        car's file, copy the new car's file over config.json, live
 *        reload, SPECIFIC init, event), the new-car path with LRU
 *        eviction, and the detection job's store update including the
 *        one-time chip protocol save. INTERNAL-stack callers only (the
 *        scan task, the httpd task, the `apid_veh` worker).
 */
#include "autopid_private.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "filesystem.h"
#include "obd_chip.h"

#include "autopid.h"                /* autopid_reload_config()          */

static const char *TAG = "autopid";

/* ---- tables files ---------------------------------------------------------------- */

/** The whole file in a PSRAM heap buffer (NUL-terminated; caller frees).
 *  NULL when missing, empty or out of memory. */
static char *read_whole(const char *path, size_t *len_out)
{
    size_t size = 0;

    *len_out = 0;

    if (filesystem_size(path, &size) != ESP_OK || size == 0)
    {
        return NULL;
    }

    char *buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        return NULL;
    }

    size_t got = 0;

    if (filesystem_read(path, buf, size, &got) != ESP_OK)
    {
        free(buf);
        return NULL;
    }

    buf[got] = '\0';
    *len_out = got;
    return buf;
}

esp_err_t ap_veh_write_guarded(const char *path, const char *json,
                               size_t len)
{
    size_t have = 0;
    char *existing = read_whole(path, &have);
    bool same = (existing != NULL && have == len &&
                 memcmp(existing, json, len) == 0);

    free(existing);

    if (same)
    {
        return ESP_OK;                  /* identical bytes: no flash write */
    }

    return filesystem_write(path, json, len);
}

esp_err_t ap_veh_copy_tables(const char *src, const char *dst)
{
    size_t len = 0;
    char *buf = read_whole(src, &len);
    const char *json = (buf != NULL) ? buf : AP_VEH_EMPTY_CONFIG;
    size_t n = (buf != NULL) ? len : strlen(AP_VEH_EMPTY_CONFIG);
    esp_err_t err = ap_veh_write_guarded(dst, json, n);

    free(buf);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "copy %s -> %s failed (%s)", src, dst,
                 esp_err_to_name(err));
    }

    return err;
}

/* ---- the switch ---------------------------------------------------------------------- */

/** Snapshot the live tables into @p prev_key's file (when it is another
 *  car) so nothing a user edited is lost by the switch. */
static void snapshot_previous(const char *prev_key, const char *new_key)
{
    char path[64];

    if (prev_key == NULL || prev_key[0] == '\0' ||
        strcmp(prev_key, new_key) == 0)
    {
        return;
    }

    ap_veh_car_path(prev_key, path, sizeof(path));
    (void)ap_veh_copy_tables(autopid_config_path(), path);
}

/** The live tables changed under the poller: reload them and the
 *  current car's SPECIFIC init (both callable from any internal task). */
static void reload_live(void)
{
    esp_err_t err = autopid_reload_config();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "vehicle switch: config reload failed (%s)",
                 esp_err_to_name(err));
    }

    ap_vehicle_apply_type_init();
    ap_runner_reset();
    ap_runner_rebaseline();
}

void ap_veh_switch_files(const char *prev_key, bool known)
{
    ap_veh_entry_t cur;
    char path[64];

    ap_veh_lock();

    ap_veh_index_t *idx = ap_veh_index();

    if (idx->current < 0 || idx->current >= idx->n)
    {
        ap_veh_unlock();
        return;
    }

    cur = idx->v[idx->current];
    ap_veh_unlock();

    snapshot_previous(prev_key, cur.key);
    ap_veh_car_path(cur.key, path, sizeof(path));
    (void)ap_veh_copy_tables(path, autopid_config_path());
    reload_live();
    (void)ap_veh_persist();

    ESP_LOGI(TAG, "vehicle switched to %s (%s), protocol %s", cur.name,
             cur.key, cur.protocol[0] ? cur.protocol : "(none)");
    ap_events_vehicle_changed(cur.vin, cur.name, known);
}

void ap_veh_new_car_files(const char *prev_key, const char *new_key,
                          const char *std_config, size_t len)
{
    snapshot_previous(prev_key, new_key);

    /* autopid_config_save() writes config.json AND mirrors it into the
       current car's file (ap_vehicle_config_saved): both tables at once */
    esp_err_t err = autopid_config_save(std_config, len);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "new vehicle %s: tables not written (%s)", new_key,
                 esp_err_to_name(err));
    }

    reload_live();
}

/* ---- the detection job's result (scan task) ------------------------------------------ */

/** The chip learns the base protocol once: a REAL ATSP through the
 *  driver's save path, only when the car's record says the chip holds
 *  something else, at most once per boot (the driver enforces it). */
static void learn_chip_protocol(const char *key, const char *proto)
{
    bool need = false;

    if (proto == NULL || proto[0] == '\0')
    {
        return;
    }

    ap_veh_lock();

    int i = ap_vidx_find_key(ap_veh_index(), key);

    if (i >= 0)
    {
        need = (strcmp(ap_veh_index()->v[i].chip_protocol, proto) != 0);
    }

    ap_veh_unlock();

    if (!need)
    {
        return;
    }

    esp_err_t err = obd_chip_protocol_save(proto[0]);

    if (err == ESP_OK)
    {
        ap_veh_lock();
        i = ap_vidx_find_key(ap_veh_index(), key);

        if (i >= 0)
        {
            snprintf(ap_veh_index()->v[i].chip_protocol,
                     sizeof(ap_veh_index()->v[i].chip_protocol), "%s", proto);
        }

        ap_veh_unlock();
        (void)ap_veh_persist();
        ESP_LOGI(TAG, "vehicle %s: chip base protocol saved (ATSP%s)", key,
                 proto);
    }
    else if (err == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGI(TAG, "vehicle %s: chip protocol save deferred (one per "
                      "boot already done)", key);
    }
    else
    {
        ESP_LOGW(TAG, "vehicle %s: chip protocol save failed (%s)", key,
                 esp_err_to_name(err));
    }
}

esp_err_t autopid_vehicle_detected(const ap_veh_seen_t *seen,
                                   const char *std_config, size_t len,
                                   ap_veh_entry_t *out_entry,
                                   bool *out_known)
{
    /* detection-job (scan task) only: the three entries would be ~1 KB
       of its 6 KB internal stack above the config reload it runs */
    static ap_veh_entry_t result EXT_RAM_BSS_ATTR;
    static ap_veh_entry_t evicted EXT_RAM_BSS_ATTR;
    static ap_veh_entry_t ne EXT_RAM_BSS_ATTR;
    char fp[AP_FP_LEN] = "";
    char prev[AP_VEH_KEY_LEN] = "";
    bool known = false;
    bool was_current = false;
    int64_t now = ap_veh_epoch_now();

    if (seen == NULL || out_entry == NULL || out_known == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ap_veh_fingerprint(seen->ecus, seen->n_ecus, fp);

    if (seen->vin[0] == '\0' && fp[0] == '\0')
    {
        return ESP_ERR_NOT_FOUND;       /* nothing identifiable answered  */
    }

    memset(&evicted, 0, sizeof(evicted));
    ap_veh_lock();

    ap_veh_index_t *idx = ap_veh_index();

    if (idx->current >= 0 && idx->current < idx->n)
    {
        memcpy(prev, idx->v[idx->current].key, sizeof(prev));
    }

    int i = ap_vidx_match(idx, seen->vin, seen->ecus, seen->n_ecus, fp, NULL);

    if (i >= 0)
    {
        ap_veh_entry_t *e = &idx->v[i];

        known = true;
        was_current = (i == idx->current);
        (void)ap_veh_refine_entry(e, seen, fp);
        e->std_supported = seen->std_supported;
        e->scan_ts = now;

        if (now > 0)
        {
            e->last_seen = now;

            if (e->first_seen == 0)
            {
                e->first_seen = now;
            }
        }
    }
    else
    {
        memset(&ne, 0, sizeof(ne));
        ap_vidx_key_for(seen->vin, fp, ne.key);
        snprintf(ne.vin, sizeof(ne.vin), "%s", seen->vin);
        snprintf(ne.fingerprint, sizeof(ne.fingerprint), "%s", fp);
        memcpy(ne.ecus, seen->ecus, sizeof(ne.ecus));
        ne.n_ecus = seen->n_ecus;
        ap_vidx_default_name(seen->vin, fp, ne.name);
        snprintf(ne.protocol, sizeof(ne.protocol), "%s", seen->protocol);
        ne.std_supported = seen->std_supported;
        ne.pending_profile = true;
        ne.first_seen = now;
        ne.last_seen = now;
        ne.scan_ts = now;

        i = ap_vidx_add(idx, &ne, &evicted);

        if (i < 0)
        {
            ap_veh_unlock();
            ESP_LOGW(TAG, "vehicle store: could not add %s", ne.key);
            return ESP_FAIL;
        }
    }

    idx->current = (int8_t)i;
    ap_veh_refresh_protocol_cache();
    result = idx->v[i];
    ap_veh_unlock();

    if (evicted.key[0] != '\0')
    {
        char path[64];

        ESP_LOGW(TAG, "vehicle store full (%d): forgot %s (%s), last seen "
                      "%lld", AP_VEH_MAX, evicted.name, evicted.key,
                 (long long)evicted.last_seen);
        ap_veh_car_path(evicted.key, path, sizeof(path));
        (void)filesystem_delete(path);
        ap_events_vehicle_evicted(evicted.vin, evicted.name);
    }

    if (known && was_current)
    {
        (void)ap_veh_persist();         /* a re-run on the current car    */
    }
    else if (known)
    {
        ap_veh_switch_files(prev, true);
    }
    else
    {
        ap_veh_new_car_files(prev, result.key, std_config, len);
        (void)ap_veh_persist();
        ESP_LOGI(TAG, "new vehicle %s (%s): %u standard PIDs, protocol %s, "
                      "profile pending", result.name, result.key,
                 (unsigned)result.std_supported,
                 result.protocol[0] ? result.protocol : "(none)");
        ap_events_vehicle_changed(result.vin, result.name, false);
    }

    learn_chip_protocol(result.key, seen->protocol);

    /* the caller's copy carries the chip_protocol the save just set */
    ap_veh_lock();
    i = ap_vidx_find_key(ap_veh_index(), result.key);

    if (i >= 0)
    {
        result = ap_veh_index()->v[i];
    }

    ap_veh_unlock();

    *out_entry = result;
    *out_known = known;
    return ESP_OK;
}
