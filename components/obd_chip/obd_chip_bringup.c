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
 * @file obd_chip_bringup.c
 * @brief The chip's boot sequence on the bare UART (before the RX task
 *        exists): wake, reset, baud negotiation, the legacy provisioning
 *        (sleep control, power-on baud, the power-pin switch). Split out
 *        of obd_chip.c 2026-10-03 (700-line rule); obd_chip.c keeps the
 *        bring-up task and its gate.
 */
#include "obd_chip.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define OBD_INIT_RETRIES 3

/** One request->response on the bare UART (RX task not running yet). */
static bool bare_probe(const char *cmd, char *resp, size_t resp_len,
                       uint32_t timeout_ms)
{
    /* static: the accumulator's 4 KB response buffer was over half the
       8 KB main-task stack at boot (2026-07-22 stack audit). Safe as a
       static — bare_probe is boot-provisioning only, before the RX
       task exists, single-threaded by construction. */
    static obd_resp_acc_t acc EXT_RAM_BSS_ATTR;
    uint8_t buf[OBD_CHIP_CHUNK_SIZE];

    obd_uart_flush_input();
    obd_uart_write((const uint8_t *)cmd, strlen(cmd));
    obd_parse_reset(&acc);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (!acc.done && xTaskGetTickCount() < deadline)
    {
        int got = obd_uart_read(buf, sizeof(buf), 50);

        if (got > 0)
        {
            obd_parse_feed(&acc, (const char *)buf, (size_t)got);
        }
    }

    if (acc.done)
    {
        obd_parse_extract(&acc, cmd, resp, resp_len);
        return true;
    }

    return false;
}

/**
 * Boot chip provisioning (legacy main/obd.c obd_init, meatpi 2026-07-04:
 * "follow legacy init"): read STSLCS; ensure sleep control is NATIVE
 * (ATPP 0E) and the stored VL thresholds/time match settings — with all
 * autonomous controls left OFF (the future sleep_manager arms them);
 * persist the 2M default baud (ATPP 0F SV 95). Any change ends in ATZ +
 * re-bring-up. Runs on the bare UART, echo already off. Log-and-degrade.
 */
static void chip_provision(const obd_config_t *cfg)
{
    static char resp[1024] EXT_RAM_BSS_ATTR; /* STSLCS is multi-line */
    obd_stslcs_t st;
    char cmd[48];
    bool reset_needed = false;

    if (!bare_probe("STSLCS\r", resp, sizeof(resp), 1500))
    {
        ESP_LOGW(TAG, "STSLCS: no answer; skipping chip provisioning");
        return;
    }

    obd_stslcs_parse(resp, &st);

    if (strcmp(st.ctrl_mode, "ELM327") == 0)
    {
        /* legacy: sleep control must be Native (the ESP is the master) */
        ESP_LOGI(TAG, "chip sleep control ELM327 -> Native");
        bare_probe("ATPP 0E SV 7A\r", resp, sizeof(resp), 1000);
        bare_probe("ATPP 0E ON\r", resp, sizeof(resp), 1000);
        reset_needed = true;
    }

    if (obd_stslcs_needs_provision(&st, cfg->wake_voltage,
                                   cfg->sleep_voltage, cfg->sleep_time_s))
    {
        ESP_LOGI(TAG, "provisioning sleep config (wake >%.2fV, sleep "
                 "<%.2fV for %lus, controls off)", cfg->wake_voltage,
                 cfg->sleep_voltage, (unsigned long)cfg->sleep_time_s);
        snprintf(cmd, sizeof(cmd), "STSLVLW >%.2f, 1\r",
                 cfg->wake_voltage);
        bare_probe(cmd, resp, sizeof(resp), 1000);
        snprintf(cmd, sizeof(cmd), "STSLVLS <%.2f, %lu\r",
                 cfg->sleep_voltage, (unsigned long)cfg->sleep_time_s);
        bare_probe(cmd, resp, sizeof(resp), 1000);
        /* byte-exact legacy strings, including the odd casing/space */
        bare_probe("STSLVl off,off\r", resp, sizeof(resp), 1000);
        bare_probe("STSLU off, off\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));

        /* persist 2M as the chip's power-on baud (legacy PP value 0x95;
           the baud is fixed at OBD_CHIP_BAUD since 2026-09-07) */
        _Static_assert(OBD_CHIP_BAUD == 2000000, "PP 0F SV 95 encodes 2 Mbaud");
        bare_probe("ATPP 0F SV 95\r", resp, sizeof(resp), 1000);
        bare_probe("ATPP 0F ON\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));

        bare_probe("STSLUIT 1200\r", resp, sizeof(resp), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        reset_needed = true;
    }

    if (reset_needed)
    {
        /* ATZ reboots the chip at its (possibly new) default baud with
           echo back on — re-run the tail of the bring-up */
        obd_uart_write((const uint8_t *)"ATZ\r", 4);
        vTaskDelay(pdMS_TO_TICKS(1500));
        obd_uart_set_baud(OBD_CHIP_BAUD);
        obd_uart_write((const uint8_t *)"\r", 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_flush_input();

        if (!bare_probe("ATE0\r", resp, sizeof(resp), 1500))
        {
            obd_uart_set_baud(115200); /* chip kept its old default */
            obd_uart_write((const uint8_t *)"\r", 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            obd_uart_flush_input();

            if (bare_probe("ATE0\r", resp, sizeof(resp), 1500))
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

/**
 * Reset an awake chip by command, and make sure it happened.
 *
 * Until 2026-10-03 this was one `ATZ` whose prompt was taken as proof. On a
 * software restart of the ESP the chip usually answered that first command
 * with a prompt and no reset (what the last run left in its line buffer, or
 * a glitch of our TX pin, sat in front of it): bring-up took 852 ms instead
 * of 2096 ms on 15 of 30 logged boots, and the chip kept everything the
 * previous run had set, an open CAN session included. A chip holding a
 * session at 500 kbit/s is a node at 500 kbit/s: on a bus at another bitrate
 * it destroys the traffic (bench: 450 error frames a second on a 250k bus,
 * from a device whose firmware believed the chip was fresh).
 *
 * So: one throw-away command first (whatever is in front of it ends with
 * it, as a `?`), then `ATZ`, and the answer must carry the banner. Twice;
 * the caller pulses the reset pin when that fails.
 */
static bool chip_soft_reset(char *resp, size_t resp_len)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        (void)bare_probe("ATI\r", resp, resp_len, 500);

        if (bare_probe("ATZ\r", resp, resp_len, 2500) &&
            strstr(resp, "ELM") != NULL)
        {
            return true;
        }
    }

    return false;
}

/** The full wake/reset/negotiate/provision sequence — the pre-2026-07-26
 *  synchronous obd_chip_start() body, run by the bring-up task in
 *  obd_chip.c. @p active_baud: the baud the chip answers at (written as the
 *  sequence learns it). ESP_OK = the chip is up and the RX task runs. */
esp_err_t obd_bringup_run(int *active_baud)
{
    const obd_config_t *cfg = obd_settings_config();
    char resp[64];

    obd_pin_wake();
    vTaskDelay(pdMS_TO_TICKS(300));

    /* legacy elm327_hardreset_chip: hardware reset ONLY when the READY
       pin says asleep/stuck (HIGH); an awake chip gets a soft ATZ */
    obd_uart_set_baud(OBD_CHIP_BAUD);

    if (obd_pin_ready())
    {
        ESP_LOGI(TAG, "chip awake; soft ATZ instead of hardware reset");

        if (!chip_soft_reset(resp, sizeof(resp)))
        {
            ESP_LOGW(TAG, "soft reset not confirmed; hardware reset");
            obd_pin_reset_pulse();
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    else
    {
        ESP_LOGW(TAG, "chip not ready; hardware reset");
        obd_pin_reset_pulse();
        vTaskDelay(pdMS_TO_TICKS(1500)); /* chip boot time after reset */
    }

    /* baud negotiation: the fixed baud first, then the chip's power-on default */
    static const int FALLBACK_BAUD = 115200;
    const int try_bauds[2] = { OBD_CHIP_BAUD, FALLBACK_BAUD };
    bool alive = false;

    for (size_t b = 0; b < 2 && !alive; b++)
    {
        obd_uart_set_baud(try_bauds[b]);

        for (int attempt = 0; attempt < OBD_INIT_RETRIES && !alive; attempt++)
        {
            /* lone CR clears any stale/monitor state (stop byte is space,
               but on a fresh boot CR-flush is what the legacy flow does) */
            obd_uart_write((const uint8_t *)"\r", 1);
            vTaskDelay(pdMS_TO_TICKS(100));

            alive = bare_probe("ATI\r", resp, sizeof(resp), 1500);
        }

        if (alive)
        {
            *active_baud = try_bauds[b];
        }
    }

    if (!alive)
    {
        ESP_LOGE(TAG, "chip not responding at %d or %d baud", OBD_CHIP_BAUD,
                 FALLBACK_BAUD);
        return ESP_ERR_NOT_FOUND;
    }

    /* switch the chip to the fixed baud if it woke at the fallback */
    if (*active_baud != OBD_CHIP_BAUD)
    {
        char cmd[24];

        snprintf(cmd, sizeof(cmd), "STSBR %d\r", OBD_CHIP_BAUD);
        obd_uart_write((const uint8_t *)cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_set_baud(OBD_CHIP_BAUD);

        if (bare_probe("ATI\r", resp, sizeof(resp), 1500))
        {
            *active_baud = OBD_CHIP_BAUD;
            /* legacy: persist the negotiated baud as the chip's
               power-on default (next boot answers at OBD_CHIP_BAUD) */
            bare_probe("STWBR\r", resp, sizeof(resp), 1500);
        }
        else
        {
            obd_uart_set_baud(FALLBACK_BAUD); /* stay where the chip is */
            *active_baud = FALLBACK_BAUD;
            ESP_LOGW(TAG, "STSBR to %d failed; staying at %d", OBD_CHIP_BAUD,
                     FALLBACK_BAUD);
        }
    }

    /* legacy elm327_powerpin_commands: fw >= V2.3.22 must run with the
       power-pin switch config at 10 */
    if (bare_probe("VTVERS\r", resp, sizeof(resp), 1500) &&
        strstr(resp, "V2.3.22") != NULL &&
        bare_probe("VTPPSWS\r", resp, sizeof(resp), 1500) &&
        strstr(resp, "10") == NULL)
    {
        ESP_LOGW(TAG, "PPSW is not 10, setting to 10");
        bare_probe("VTPPSW10\r", resp, sizeof(resp), 1500);
        obd_pin_reset_pulse(); /* legacy hard-resets after the change */
        vTaskDelay(pdMS_TO_TICKS(1500));
        obd_uart_write((const uint8_t *)"\r", 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_flush_input();
    }

    /* legacy: wait for the READY pin (LOW = ready) before declaring the
       chip up; one forced reset if it never settles */
    for (int i = 0; !obd_pin_ready() && i < 10; i++)
    {
        ESP_LOGW(TAG, "chip not ready...");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!obd_pin_ready())
    {
        ESP_LOGE(TAG, "chip not ready for too long; hardware reset");
        obd_pin_reset_pulse();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* echo off first (deterministic responses), then the legacy chip
       provisioning (task §11-5 answered by meatpi: "follow legacy init") */
    bare_probe("ATE0\r", resp, sizeof(resp), 1000);
    chip_provision(cfg);
    bare_probe("ATI\r", resp, sizeof(resp), 1500); /* identity for the log */

    esp_err_t err = obd_uart_rx_task_start();

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "started: chip '%s' at %d baud (ready=%d)", resp,
             *active_baud, obd_pin_ready());
    return ESP_OK;
}
