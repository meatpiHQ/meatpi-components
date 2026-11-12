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
 * @file obd_chip_provision.c
 * @brief The legacy provisioning of the chip's sleep control, on the bare
 *        UART behind the bring-up's reset. Moved out of obd_chip_bringup.c
 *        on 2026-11-12 (the 700-line rule), unchanged.
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

/**
 * Boot chip provisioning (legacy main/obd.c obd_init, meatpi 2026-07-04:
 * "follow legacy init"): read STSLCS; ensure sleep control is NATIVE
 * (ATPP 0E) and every autonomous control OFF (the ESP32 owns the sleep
 * policy); keep the chip from waking itself on bus activity while the
 * ESP32 sleeps (ATPP 0F SV 95: the Activity Monitor options with bit 6
 * off). Any change ends in ATZ + re-bring-up. Runs on the bare UART, echo
 * already off. Log-and-degrade.
 *
 * Until 2026-11-07 the block also wrote the voltage thresholds and the
 * UART timeout from the obd_chip settings (STSLVLW, STSLVLS, STSLUIT) and
 * re-provisioned when the stored values differed from them. The sleep
 * triggers bench (the app repo's testing/sleeptrig.md) proved those values
 * inert while the controls are off, so they are neither compared nor
 * written any more: fewer EEPROM writes, and a settings number nobody sees
 * (the UI never shows them) no longer costs a provisioning. Whatever the
 * chip holds there stays as it is.
 */
void obd_bringup_provision(void)
{
    static char resp[1024] EXT_RAM_BSS_ATTR; /* STSLCS is multi-line */
    obd_stslcs_t st;
    bool reset_needed = false;
    bool pp0f_written = false;

    if (!obd_bare_probe("STSLCS\r", resp, sizeof(resp), 1500))
    {
        ESP_LOGW(TAG, "STSLCS: no answer; skipping chip provisioning");
        return;
    }

    obd_stslcs_parse(resp, &st);

    if (strcmp(st.ctrl_mode, "ELM327") == 0)
    {
        /* legacy: sleep control must be Native (the ESP is the master) */
        ESP_LOGI(TAG, "chip sleep control ELM327 -> Native");
        obd_bare_probe("ATPP 0E SV 7A\r", resp, sizeof(resp), 1000);
        obd_bare_probe("ATPP 0E ON\r", resp, sizeof(resp), 1000);
        reset_needed = true;
    }

    if (obd_stslcs_needs_provision(&st))
    {
        ESP_LOGI(TAG, "provisioning sleep config: a control is on (uart "
                 "sleep %d wake %d, voltage sleep %d wake %d); all four off",
                 st.uart_sleep.en, st.uart_wake.en, st.vl_sleep.en,
                 st.vl_wake.en);
        /* byte-exact legacy strings, including the odd casing/space */
        obd_bare_probe("STSLVl off,off\r", resp, sizeof(resp), 1000);
        obd_bare_probe("STSLU off, off\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        pp0f_written = true;

        /* PP 0F = the Activity Monitor options (MIC DS PP area, default
           D5): 95 clears bit 6, "allow wake from Low Power on a shift from
           no activity to activity", so the chip cannot wake itself on bus
           traffic while the ESP32 sleeps (legacy obd_init and its
           elm327_disable_wake_commands). Not the baud: the 2 M power-on
           baud is STWBR's, at the 115200 fallback above. A chip firmware
           rewrite keeps the PP area (bench 2026-11-02). */
        obd_bare_probe("ATPP 0F SV 95\r", resp, sizeof(resp), 1000);
        obd_bare_probe("ATPP 0F ON\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        reset_needed = true;
    }

    /* PP 0F guarded by its own read-back (Ali, 2026-11-02): the compare
       above sees STSLCS, which does not report the Activity Monitor
       options, so a chip image or a tool that put PP 0F back to the
       vendor's D5 (the chip waking itself on bus activity while the ESP32
       sleeps) would stay that way. ATPPS is the one read the chip offers;
       the pair goes out only when the value or its enable differs (the
       boot-path change guard of the standard's section 11). An intact chip
       costs one read of 12 lines and no write; a chip firmware rewrite
       keeps the PP area (bench 2026-11-02), so the write is for the image
       or tool that does not. */
    if (!pp0f_written)
    {
        uint8_t pp0f = 0;
        bool pp0f_on = false;

        if (!obd_bare_probe("ATPPS\r", resp, sizeof(resp), 1500))
        {
            ESP_LOGW(TAG, "ATPPS: no answer; PP 0F not checked");
        }
        else if (!obd_parse_pp(resp, 0x0F, &pp0f, &pp0f_on))
        {
            ESP_LOGW(TAG, "ATPPS: no PP 0F in the answer ('%.24s'); not "
                          "checked", resp);
        }
        else if (pp0f != 0x95 || !pp0f_on)
        {
            ESP_LOGW(TAG, "PP 0F reads %02X %s (expected 95 on): restoring",
                     pp0f, pp0f_on ? "on" : "off");
            obd_bare_probe("ATPP 0F SV 95\r", resp, sizeof(resp), 1000);
            obd_bare_probe("ATPP 0F ON\r", resp, sizeof(resp), 1000);
            vTaskDelay(pdMS_TO_TICKS(10));
            obd_core_pp_restore_note();
            reset_needed = true;
        }
    }

    if (reset_needed)
    {
        /* ATZ reboots the chip at its (possibly new) default baud with
           echo back on: re-run the tail of the bring-up */
        obd_uart_write((const uint8_t *)"ATZ\r", 4);
        vTaskDelay(pdMS_TO_TICKS(1500));
        obd_uart_set_baud(OBD_CHIP_BAUD);
        obd_uart_write((const uint8_t *)"\r", 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_flush_input();

        if (!obd_bare_probe("ATE0\r", resp, sizeof(resp), 1500))
        {
            obd_uart_set_baud(115200); /* chip kept its old default */
            obd_uart_write((const uint8_t *)"\r", 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            obd_uart_flush_input();

            if (obd_bare_probe("ATE0\r", resp, sizeof(resp), 1500))
            {
                ESP_LOGW(TAG, "chip at 115200 after provisioning ATZ");
            }
            else
            {
                ESP_LOGE(TAG, "chip unresponsive after provisioning ATZ");
            }
        }
    }
}
