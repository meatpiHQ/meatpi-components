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
 * @file external_storage.c
 * @brief The SD/MMC device owner: detect-pin supervision (hot-plug),
 *        SDMMC 4-bit host bring-up, FATFS mount at /sd, dev-status bit +
 *        event callback. Settings live in external_storage_settings.c
 *        (standard §4.1).
 */
#include "external_storage.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"

#include "dev_status_manager.h"
#include "log_manager.h"

#include "external_storage_private.h"

static const char *TAG = "external_storage";

/* WiCAN Pro wiring (meatpi 2026-07-04) */
#define SDCARD_CLK        21
#define SDCARD_CMD        47
#define SDCARD_D0         14
#define SDCARD_D1         13
#define SDCARD_D2         12
#define SDCARD_D3         48
#define SDCARD_DETECT_PIN 40 /* microSD socket switch: LOW = card present */

#define ES_MOUNT_POINT    "/sd" /* == filesystem's FS_PREFIX_SD           */
#define ES_POLL_MS        250

static sdmmc_card_t *s_card;
static volatile bool s_mounted;
static volatile bool s_present;
static external_storage_event_cb_t s_cb;
static bool s_inited;

static TaskHandle_t s_task;
static StaticTask_t s_tcb; /* internal: FreeRTOS object */
/* SD I/O goes through the driver's own DMA buffers; this task performs no
 * flash-cache-off work, so a PSRAM stack is compliant (§2) */
static StackType_t s_stack[4096] EXT_RAM_BSS_ATTR;
static volatile bool s_running;

/* ---- mount / unmount ----------------------------------------------------------- */

static bool read_present(void)
{
    return gpio_get_level(SDCARD_DETECT_PIN) == 0; /* active low */
}

static void card_mount(void)
{
    if (s_mounted)
    {
        return;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* 40 MHz; driver derates */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    slot.clk = SDCARD_CLK;
    slot.cmd = SDCARD_CMD;
    slot.d0 = SDCARD_D0;
    slot.d1 = SDCARD_D1;
    slot.d2 = SDCARD_D2;
    slot.d3 = SDCARD_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg =
    {
        .format_if_mount_failed = false, /* it's the USER'S card */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(ES_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mount failed: %s (card present but unreadable?)",
                 esp_err_to_name(err));
        return;
    }

    s_mounted = true;
    dev_status_manager_set(DEV_STATUS_BIT_SDCARD_MOUNTED);
    ESP_LOGI(TAG, "mounted %s: %s %lluMB", ES_MOUNT_POINT, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) /
                 (1024 * 1024));

    if (s_cb != NULL)
    {
        s_cb(true);
    }
}

bool es_card_details(es_card_details_t *out)
{
    if (!s_mounted || s_card == NULL || out == NULL)
    {
        return false;
    }

    snprintf(out->name, sizeof(out->name), "%s", s_card->cid.name);
    out->type = s_card->is_sdio ? "SDIO"
                : s_card->is_mmc ? "MMC"
                : (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
    out->capacity_bytes =
        (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    out->sector_size = s_card->csd.sector_size;
    out->speed_khz = (uint32_t)s_card->max_freq_khz;
    return true;
}

static void card_unmount(void)
{
    if (!s_mounted)
    {
        return;
    }

    /* callback first: consumers stop routing /sd before the rug moves */
    if (s_cb != NULL)
    {
        s_cb(false);
    }

    esp_vfs_fat_sdcard_unmount(ES_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    dev_status_manager_clear(DEV_STATUS_BIT_SDCARD_MOUNTED);
    ESP_LOGI(TAG, "unmounted");
}

/* ---- detect task ----------------------------------------------------------------- */

static void detect_task(void *arg)
{
    es_detect_t det;

    (void)arg;
    es_detect_init(&det, read_present());
    s_present = det.stable_present;

    if (s_present)
    {
        card_mount(); /* card already seated at boot */
    }

    while (s_running)
    {
        vTaskDelay(pdMS_TO_TICKS(ES_POLL_MS));

        if (!es_detect_feed(&det, read_present()))
        {
            continue;
        }

        s_present = det.stable_present;
        ESP_LOGI(TAG, "card %s", s_present ? "inserted" : "removed");

        if (s_present)
        {
            card_mount();
        }
        else
        {
            card_unmount();
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle --------------------------------------------------------------------- */

esp_err_t external_storage_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC =
        { "external_storage", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    gpio_reset_pin(SDCARD_DETECT_PIN);
    gpio_set_direction(SDCARD_DETECT_PIN, GPIO_MODE_INPUT);
    gpio_pullup_en(SDCARD_DETECT_PIN); /* socket switch shorts to GND */

    s_inited = true;
    return ESP_OK;
}

esp_err_t external_storage_start(void)
{
    if (!s_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
    }

    s_running = true;
    s_task = xTaskCreateStatic(detect_task, "sd_detect",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               5, s_stack, &s_tcb);
    return (s_task != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t external_storage_stop(void)
{
    s_running = false;

    for (int i = 0; i < 25 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    card_unmount();
    return ESP_OK;
}

/* ---- status / registration ----------------------------------------------------------- */

bool external_storage_is_present(void)
{
    return s_present;
}

bool external_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t external_storage_set_callback(external_storage_event_cb_t cb)
{
    s_cb = cb;
    return ESP_OK;
}
