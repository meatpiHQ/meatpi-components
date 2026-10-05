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
 * @file restart_tracker_report.c
 * @brief The stored crash report's target half: one NVS blob.
 *
 * A crash note lives in RTC memory and goes with the power. The boot that
 * files one stores it here as well, so that a device that was unplugged
 * can still say where it crashed: on the Status page, over
 * GET /api/restart/report, on the console, and in safe mode, which opens
 * NVS and nothing else.
 *
 * NVS and not a file: the write has to happen in restart_tracker_init(),
 * the second thing a boot does, before any filesystem is mounted and before
 * the crash-loop brake may park the device; and safe mode mounts none on
 * purpose. When the write happens at all is decided by the pure half
 * (restart_tracker_report_core.c, the wear guard): standard 11.
 *
 * Flash: only the boot task (init), the console and the HTTP worker reach
 * the writers here, all three on internal stacks.
 */
#include "restart_tracker.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "restart_tracker_private.h"

static const char *TAG = "restart_tracker";

#define RT_NVS_NAMESPACE "rt_crash"
#define RT_NVS_KEY       "report"

/* The stored report as this boot knows it: what NVS held, or what this boot
   wrote. PSRAM: read by the HTTP worker and the console. */
static rt_report_t s_report EXT_RAM_BSS_ATTR;
static bool s_have;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock */

static esp_err_t nvs_load(rt_report_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RT_NVS_NAMESPACE, NVS_READONLY, &h);

    if (err != ESP_OK)
    {
        return err; /* no namespace yet: nothing was ever stored */
    }

    size_t len = sizeof(*out);

    err = nvs_get_blob(h, RT_NVS_KEY, out, &len);
    nvs_close(h);

    if (err != ESP_OK)
    {
        return err; /* no key, or a blob of another size */
    }

    return (len == sizeof(*out) && rt_report_valid(out)) ? ESP_OK
                                                         : ESP_ERR_INVALID_CRC;
}

static esp_err_t nvs_store(const rt_report_t *report)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RT_NVS_NAMESPACE, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_blob(h, RT_NVS_KEY, report, sizeof(*report));

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

static void keep(const rt_report_t *report)
{
    portENTER_CRITICAL(&s_lock);
    s_report = *report;
    s_have = true;
    portEXIT_CRITICAL(&s_lock);
}

static bool held(rt_report_t *out)
{
    portENTER_CRITICAL(&s_lock);

    bool have = s_have;

    if (have)
    {
        *out = s_report;
    }

    portEXIT_CRITICAL(&s_lock);
    return have;
}

void rt_report_boot(const rt_crash_note_t *note,
                    const restart_tracker_record_t *record,
                    const restart_tracker_brake_t *brake)
{
    /* internal: the boot task's stack; what NVS reads into and writes from */
    rt_report_t stored;
    rt_report_t fresh;
    bool have = nvs_load(&stored) == ESP_OK;

    if (have)
    {
        keep(&stored);
    }

    rt_brake_state_t *count = rt_crash_brake_state();

    if (note == NULL || count == NULL)
    {
        return; /* no crash before this boot: flash is not touched */
    }

    /* the version string belongs in the report only when the image that
       crashed is the one running now, which the ELF id says */
    const char *firmware = "";

    if (rt_crash_tail_valid(note) &&
        strncmp(note->elf, esp_app_get_elf_sha256_str(), RT_CRASH_ELF_LEN) == 0)
    {
        firmware = esp_app_get_description()->version;
    }

    rt_report_build(&fresh, note,
                    (record->time_valid != 0U) ? record->boot_timestamp : 0,
                    firmware, brake->streak,
                    brake->verdict != RESTART_TRACKER_BOOT_NORMAL);

    switch (rt_report_decide(have ? &stored : NULL, &fresh, count->budget))
    {
        case RT_REPORT_STORE:
        {
            esp_err_t err = nvs_store(&fresh);

            if (err != ESP_OK)
            {
                /* W: the boot goes on without it, as it would without NVS */
                ESP_LOGW(TAG, "crash report not stored: %s",
                         esp_err_to_name(err));
                break;
            }

            keep(&fresh);
            (void)rt_brake_spend(count);
            ESP_LOGW(TAG, "crash report stored (%u more until a run settles)",
                     (unsigned)count->budget);
            break;
        }

        case RT_REPORT_NO_BUDGET:
            ESP_LOGW(TAG, "crash report not stored: %u were since a run last "
                     "settled", (unsigned)RT_REPORT_BUDGET);
            break;

        default:
            ESP_LOGI(TAG, "crash report: the stored one is this crash");
            break;
    }
}

esp_err_t restart_tracker_get_report(restart_tracker_report_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    rt_report_t report;

    if (!held(&report))
    {
        return ESP_ERR_NOT_FOUND;
    }

    rt_report_export(&report, out);
    return ESP_OK;
}

esp_err_t restart_tracker_clear_report(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RT_NVS_NAMESPACE, NVS_READWRITE, &h);

    if (err == ESP_OK)
    {
        err = nvs_erase_key(h, RT_NVS_KEY);

        if (err == ESP_OK)
        {
            err = nvs_commit(h);
        }
        else if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            err = ESP_OK; /* nothing stored: nothing to write */
        }

        nvs_close(h);
    }

    if (err == ESP_OK)
    {
        portENTER_CRITICAL(&s_lock);
        s_have = false;
        portEXIT_CRITICAL(&s_lock);
    }

    return err;
}

esp_err_t restart_tracker_restore_report(void)
{
    rt_report_t report; /* internal: the caller's stack; NVS writes from it */

    if (!held(&report))
    {
        return ESP_ERR_NOT_FOUND;
    }

    return nvs_store(&report);
}

int restart_tracker_report_text(const restart_tracker_report_t *report,
                                char *buf, size_t cap)
{
    /* the device id as dev_status_manager_device_id() builds it (the SoftAP
       MAC): this component sits below that one and cannot ask it */
    uint8_t mac[6] = { 0 };
    char device[13] = "";

    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK)
    {
        snprintf(device, sizeof(device), "%02x%02x%02x%02x%02x%02x", mac[0],
                 mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    return rt_report_text(report, device, buf, cap);
}
