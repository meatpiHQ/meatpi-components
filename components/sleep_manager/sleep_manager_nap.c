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
 * @file sleep_manager_nap.c
 * @brief The board and the OBD chip through the naps: the board's
 *        low-power state, and after every nap the check that the chip is
 *        still asleep, its re-sleep and the two fault codes
 *        (sleep_manager_chip.c has the verdicts). Moved out of
 *        sleep_manager.c on 2026-11-12 (the 700-line rule) with the fault
 *        codes.
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev_status_manager.h"
#include "obd_chip.h"
#include "restart_tracker.h"

#include "sleep_manager_private.h"

static const char *TAG = "sleep_manager";

void sleep_manager_board_down(void)
{
    /* CAN transceiver to standby */
    gpio_set_direction(SM_CAN_STDBY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SM_CAN_STDBY_GPIO, 1);

    (void)obd_chip_sleep(true);

    /* keep the USB power rail off through the naps (legacy) */
    gpio_set_direction(SM_USB_PWR_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(SM_USB_PWR_GPIO, 0);
    gpio_sleep_set_pull_mode(SM_USB_PWR_GPIO, GPIO_PULLDOWN_ONLY);
    gpio_pulldown_en(SM_USB_PWR_GPIO);
    gpio_hold_en(SM_USB_PWR_GPIO);
}

void sm_nap_verify_chip(uint32_t nap, uint32_t *resleeps)
{
    /* zero = sm_chip_watch_init(): a sleep ends in a reboot, so the watch
       lives exactly as long as one sleep */
    static sm_chip_watch_t s_chip;
    char detail[48];

    /* READY reads ready = the chip is awake */
    sm_chip_verdict_t verdict = sm_chip_watch_feed(&s_chip, obd_chip_status_ok());

    if (verdict == SM_CHIP_ASLEEP)
    {
        return;
    }

    (*resleeps)++;

    if (verdict == SM_CHIP_GIVE_UP)
    {
        ESP_LOGE(TAG, "OBD chip will not sleep; recovery reboot");
        (void)dev_status_manager_fault_raise(
            SM_FAULT_CHIP_NO_SLEEP, "6 re-sleeps in a row failed: recovery reboot");
        restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_INTERNAL_RECOVERY,
                                RESTART_TRACKER_SOURCE_SLEEP_MODE, 0);
        return;
    }

    if (verdict == SM_CHIP_WOKE)
    {
        /* it had slept and is awake again, the sleep pin still low: a
           fault, latched until someone clears it (the wake's reboot throws
           the log and the counters away) */
        snprintf(detail, sizeof(detail), "awake at nap %lu after it had slept",
                 (unsigned long)nap);
        (void)dev_status_manager_fault_raise(SM_FAULT_CHIP_WOKE, detail);
    }

    ESP_LOGW(TAG, "OBD chip awake during sleep; re-sleeping (%u/%u)",
             (unsigned)s_chip.in_a_row, (unsigned)SM_RESLEEP_MAX);

    /* a reset by command FIRST: a chip mid-monitor (ATMA) ignores the sleep
       pin and the sleep word (`elm_monitor`, 2026-07-21). Legacy's re-sleep:
       a soft ATZ, STSLEEP0, the pin. Until 2026-11-12 a pulse on the reset
       line stood here: a chip that answers gets none any more (issue #1023,
       obd_chip's README). Not confirmed = the chip may have reset with its
       banner lost: its boot time, so the word finds it listening */
    if (obd_chip_soft_reset() != ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(SM_RESLEEP_BOOT_MS));
    }

    (void)obd_chip_sleep(true);
    vTaskDelay(pdMS_TO_TICKS(100));
}
