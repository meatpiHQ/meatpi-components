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
 * @file sleep_manager_update.c
 * @brief No sleep while firmware is being written (Ali, 2026-11-21, issue
 *        #1075): the OBD chip's, and the device's own image. Split out of
 *        sleep_manager.c with the entry's wait (700-line rule).
 *
 * The OBD chip's update owns the chip's wire for about 20 s (3557 records);
 * the sleep entry pulls the chip's sleep pin, and a half-written image
 * strands the chip in its loader (issue #1023, 2026-10-28). The device's
 * own image is an ota_manager session, whoever feeds it (the page's upload,
 * the update job's download): a sleep entry in the middle takes the network
 * away under it, and one between the image's last byte and the restart into
 * it puts the device to sleep on the old firmware with the new one set to
 * boot. Both are asked the same two ways: the state task holds a running
 * countdown away while one runs (the activity hold, its cap with it), and
 * the entry sequence waits for one that is still running when it begins.
 * Bounded: a wedged update must never hold a flat battery awake for good.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip.h"
#include "ota_manager.h"

#include "sleep_manager.h"
#include "sleep_manager_private.h"

static const char *TAG = "sleep_manager";

#define SM_FW_UPDATE_WAIT_MS (180 * 1000)

static const char *(*s_busy_check)(void);

void sleep_manager_set_busy_check(const char *(*fn)(void))
{
    s_busy_check = fn;
}

const char *sm_update_busy(void)
{
    if (s_busy_check != NULL && s_busy_check() != NULL)
    {
        return SM_WHY_ECU_FLASH;
    }

    if (obd_chip_update_in_progress())
    {
        return SM_WHY_CHIP_UPDATE;
    }

    if (ota_manager_busy())
    {
        return SM_WHY_FW_UPDATE;
    }

    return NULL;
}

const char *sm_update_name(const char *why)
{
    if (why == SM_WHY_ECU_FLASH)
    {
        return "ECU flash";
    }

    return (why == SM_WHY_FW_UPDATE) ? "firmware update"
                                     : "OBD chip firmware update";
}

void sm_update_wait(void)
{
    const char *why = sm_update_busy();

    if (why == NULL)
    {
        return;
    }

    ESP_LOGW(TAG, "%s in progress: sleep waits for it (up to %u s)",
             sm_update_name(why), SM_FW_UPDATE_WAIT_MS / 1000u);

    for (uint32_t waited = 0;
         sm_update_busy() != NULL && waited < SM_FW_UPDATE_WAIT_MS;
         waited += 1000)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
