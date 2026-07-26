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
 * @file dev_status_faults.c
 * @brief Device fault codes — the automotive-DTC idea applied to the
 *        firmware itself (meatpi 2026-07-19): when a structural problem
 *        fires (registry overflow, settings degraded to defaults, errors
 *        during boot, ...), it is LATCHED to NVS and survives reboots and
 *        power cycles until MANUALLY cleared (`faults -c` /
 *        POST /api/faults/clear). A transient that happened once in the
 *        field is still visible on the bench weeks later.
 *
 * Wear discipline (this feature must never become the wear bug it
 * guards against): NVS (wear-leveled), one blob, written only on the
 * FIRST occurrence of a code and at most ONE count-update per code per
 * boot. Recurrences within a boot count in RAM only.
 */
#include "dev_status_manager.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "dev_status_manager";

#define DSM_FAULT_NVS_NS  "dfc"
#define DSM_FAULT_NVS_KEY "faults"

static dev_status_fault_t s_faults[DEV_STATUS_FAULT_MAX];
static int s_fault_count;
static uint32_t s_persisted_this_boot; /* bitmask: count-update done */
static SemaphoreHandle_t s_fault_lock;
static StaticSemaphore_t s_fault_lock_buf;

static void faults_save(void)
{
    nvs_handle_t h;

    if (nvs_open(DSM_FAULT_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        return;
    }

    if (s_fault_count == 0)
    {
        nvs_erase_key(h, DSM_FAULT_NVS_KEY);
    }
    else
    {
        nvs_set_blob(h, DSM_FAULT_NVS_KEY, s_faults,
                     sizeof(s_faults[0]) * (size_t)s_fault_count);
    }

    nvs_commit(h);
    nvs_close(h);
}

void dsm_faults_load(void)
{
    if (s_fault_lock == NULL)
    {
        s_fault_lock = xSemaphoreCreateMutexStatic(&s_fault_lock_buf);
    }

    nvs_handle_t h;

    if (nvs_open(DSM_FAULT_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return; /* fresh device: no namespace yet */
    }

    size_t len = sizeof(s_faults);

    if (nvs_get_blob(h, DSM_FAULT_NVS_KEY, s_faults, &len) == ESP_OK &&
        len % sizeof(s_faults[0]) == 0)
    {
        s_fault_count = (int)(len / sizeof(s_faults[0]));

        for (int i = 0; i < s_fault_count; i++) /* defensive NUL terms */
        {
            s_faults[i].code[sizeof(s_faults[i].code) - 1] = '\0';
            s_faults[i].detail[sizeof(s_faults[i].detail) - 1] = '\0';
        }

        ESP_LOGW(TAG, "%d latched fault code(s) present (see `faults`)",
                 s_fault_count);
    }

    nvs_close(h);
}

esp_err_t dev_status_manager_fault_raise(const char *code,
                                         const char *detail)
{
    if (code == NULL || code[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_fault_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE; /* pre-init: nothing to latch into */
    }

    xSemaphoreTake(s_fault_lock, portMAX_DELAY);

    dev_status_fault_t *f = NULL;
    bool is_new = false;

    for (int i = 0; i < s_fault_count; i++)
    {
        if (strcmp(s_faults[i].code, code) == 0)
        {
            f = &s_faults[i];
            break;
        }
    }

    if (f == NULL)
    {
        if (s_fault_count >= DEV_STATUS_FAULT_MAX)
        {
            /* full: overwrite the last slot rather than lose the newest */
            f = &s_faults[DEV_STATUS_FAULT_MAX - 1];
            memset(f, 0, sizeof(*f));
        }
        else
        {
            f = &s_faults[s_fault_count++];
        }

        strncpy(f->code, code, sizeof(f->code) - 1);
        f->first_time = (uint32_t)time(NULL);
        is_new = true;
    }

    f->count++;
    f->last_time = (uint32_t)time(NULL);

    if (detail != NULL) /* keep the LATEST detail */
    {
        strncpy(f->detail, detail, sizeof(f->detail) - 1);
        f->detail[sizeof(f->detail) - 1] = '\0';
    }

    /* wear discipline: persist new codes, and at most one count-update
       per code per boot */
    int idx = (int)(f - s_faults);
    bool persist = is_new ||
                   (s_persisted_this_boot & (1u << idx)) == 0;

    if (persist)
    {
        s_persisted_this_boot |= (1u << idx);
        faults_save();
    }

    xSemaphoreGive(s_fault_lock);

    ESP_LOGE(TAG, "FAULT %s: %s (count %lu)", code,
             (detail != NULL) ? detail : "", (unsigned long)f->count);
    return ESP_OK;
}

int dev_status_manager_faults(dev_status_fault_t *out, int cap)
{
    if (s_fault_lock == NULL)
    {
        return 0;
    }

    xSemaphoreTake(s_fault_lock, portMAX_DELAY);

    int n = (s_fault_count < cap) ? s_fault_count : cap;

    if (out != NULL)
    {
        memcpy(out, s_faults, sizeof(s_faults[0]) * (size_t)n);
    }
    else
    {
        n = s_fault_count; /* NULL out = just the count */
    }

    xSemaphoreGive(s_fault_lock);
    return n;
}

esp_err_t dev_status_manager_faults_clear(void)
{
    if (s_fault_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_fault_lock, portMAX_DELAY);
    memset(s_faults, 0, sizeof(s_faults));
    s_fault_count = 0;
    s_persisted_this_boot = 0;
    faults_save();
    xSemaphoreGive(s_fault_lock);

    ESP_LOGI(TAG, "fault codes cleared");
    return ESP_OK;
}
