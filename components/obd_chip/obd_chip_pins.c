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
 * @file obd_chip_pins.c
 * @brief The chip's three control lines: READY (the chip's output), the
 *        sleep pin (PowerEn) and the reset line, with their sleep / wake
 *        sequences (the sleep hold SURVIVES resets, so waking needs the
 *        release sequence; bench 2026-07-03). Moved out of obd_chip_uart.c
 *        on 2026-11-12 (the 700-line rule) together with the rule this
 *        file enforces:
 *
 *        THE RESET LINE IS THE LAST RESORT, FOR A CHIP THAT ANSWERS NOTHING
 *        (issue #1023). The chip tidies its store during a boot, and a reset
 *        inside that tidying destroys the store for good (obd_chip_settle.c
 *        has the measurements and the rule). obd_pin_reset_pulse() is the
 *        only code that drives the reset line: it pulses only a chip its
 *        caller has just asked at both UART speeds without an answer, and
 *        only once READY is at rest. It is in no API, and its declaration
 *        (obd_chip_reset_line.h) is included by the bring-up alone. A chip
 *        that talks is reset by command. Do not add a second way to pull
 *        that line, and do not pulse a chip that answers.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip.h"
#include "obd_chip_private.h"
#include "obd_chip_reset_line.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define OBD_READY_PIN GPIO_NUM_7 /* chip-driven; LOW = awake/ready, HIGH =
                                     asleep (legacy elm327_chip_get_status:
                                     ELM327_READY==0, task §11.1 answered) */
#define OBD_SLEEP_PIN GPIO_NUM_9  /* manager-driven; high = awake            */
#define OBD_RESET_PIN GPIO_NUM_41 /* active-low pulse; OPEN-DRAIN output
                                     (legacy init: never drive it high)     */

#define OBD_SETTLE_POLL_MS 2 /* READY's phases last 100 ms and more */

/* the crash park's sleep word: the answer's budget, and the chip's line
   buffer left to end whatever the stop byte interrupted */
#define OBD_PARK_WORD_MS 600
#define OBD_PARK_STOP_MS 100

/* the reset line's accounting (GET /api/obd_chip `reset`) */
static uint32_t s_pulses;
static uint32_t s_refused;

void obd_pins_init(void)
{
    /* the legacy init block (meatpi), the level ahead of the direction
       since 2026-11-12: the line must never see a low it was not meant to */
    gpio_reset_pin(OBD_RESET_PIN);
    gpio_set_level(OBD_RESET_PIN, 1); /* reset is active low */
    gpio_set_direction(OBD_RESET_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(OBD_RESET_PIN, 1);

    gpio_reset_pin(OBD_READY_PIN);
    gpio_set_direction(OBD_READY_PIN, GPIO_MODE_INPUT);
    gpio_pulldown_en(OBD_READY_PIN);
}

void obd_pin_wake(void)
{
    /* the sleep path parks the pin low with pulldown + gpio_hold_en + RTC
       pulldown; holds survive resets: release everything first (meatpi) */
    gpio_sleep_set_pull_mode(OBD_SLEEP_PIN, GPIO_FLOATING);
    gpio_pulldown_en(OBD_SLEEP_PIN);
    rtc_gpio_pulldown_dis(OBD_SLEEP_PIN);
    gpio_hold_dis(OBD_SLEEP_PIN);
    rtc_gpio_deinit(OBD_SLEEP_PIN);
    gpio_reset_pin(OBD_SLEEP_PIN);
    gpio_set_direction(OBD_SLEEP_PIN, GPIO_MODE_OUTPUT);
    gpio_pulldown_en(OBD_SLEEP_PIN);
    gpio_set_level(OBD_SLEEP_PIN, 1);
}

void obd_pin_sleep(void)
{
    gpio_sleep_set_pull_mode(OBD_SLEEP_PIN, GPIO_PULLDOWN_ONLY);
    gpio_set_level(OBD_SLEEP_PIN, 0);
    gpio_pulldown_en(OBD_SLEEP_PIN);
    rtc_gpio_pulldown_en(OBD_SLEEP_PIN);
    gpio_hold_en(OBD_SLEEP_PIN); /* survives resets on purpose */

    /* legacy parks the READY input too (pulled low = "asleep" reading,
       no float current during deep sleep) */
    gpio_sleep_set_pull_mode(OBD_READY_PIN, GPIO_PULLDOWN_ONLY);
    rtc_gpio_pulldown_en(OBD_READY_PIN);
    gpio_pulldown_en(OBD_READY_PIN);
    gpio_hold_en(OBD_READY_PIN);
}

bool obd_pin_ready(void)
{
    /* ACTIVE LOW: legacy elm327_chip_get_status() maps level directly
       onto {ELM327_READY = 0, ELM327_SLEEP = 1} */
    return gpio_get_level(OBD_READY_PIN) == 0;
}

obd_settle_verdict_t obd_pin_settle(uint32_t limit_ms, uint32_t *waited_ms)
{
    const int64_t t0 = esp_timer_get_time();
    obd_settle_verdict_t verdict;
    obd_settle_t st;

    obd_settle_reset(&st);

    while (true)
    {
        const int64_t now = esp_timer_get_time();

        verdict = obd_settle_feed(&st, obd_pin_ready(), now);

        if (verdict != OBD_SETTLE_MOVING || now - t0 >= (int64_t)limit_ms * 1000)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(OBD_SETTLE_POLL_MS));
    }

    if (waited_ms != NULL)
    {
        *waited_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }

    return verdict;
}

bool obd_pin_reset_pulse(bool chip_silent)
{
    /* the rule (obd_settle_pulse_allowed, issue #1023): the last resort. The
       caller asked the chip at both UART speeds and got nothing, and READY
       is at rest: ready for OBD_READY_STEADY_MS, or not ready for
       OBD_DOWN_STEADY_MS, so no boot of the chip can be running. Anything
       else is refused here, whoever asks: a chip that answers is reset by
       command, and a line that moves may belong to a chip in the middle of
       a boot. */
    uint32_t waited = 0;
    obd_settle_verdict_t settled = obd_pin_settle(OBD_PULSE_SETTLE_MS, &waited);

    if (!obd_settle_pulse_allowed(settled, chip_silent))
    {
        s_refused++;
        ESP_LOGW(TAG, "reset pulse refused: %s (READY %lu ms; the reset line "
                 "is the last resort for a chip that answers nothing)",
                 !chip_silent ? "the chip was not found silent"
                              : "READY keeps changing",
                 (unsigned long)waited);
        return false;
    }

    s_pulses++;
    gpio_set_level(OBD_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(OBD_RESET_PIN, 1);
    return true;
}

void obd_pin_get_stats(obd_pin_stats_t *out)
{
    out->pulses = s_pulses;
    out->refused = s_refused;
}

/** The park's words to an awake chip, on a UART this boot never set up: a
 *  stop byte (a chip in the middle of a monitor ignores the sleep pin; any
 *  byte ends the monitor, and a space is nothing to a chip that was idle),
 *  then the sleep word of obd_chip_sleep(true). Blind at the chip's speed:
 *  a chip at another speed still gets its monitor ended. */
static void park_sleep_word(void)
{
    static bool s_uart_up;
    static const char WORD[] = "STSLEEP0\r";
    char resp[24] = "";
    size_t len = 0;

    if (!s_uart_up)
    {
        esp_err_t err = obd_uart_init(OBD_CHIP_BAUD);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "park: no UART for the sleep word (%s): the pin alone",
                     esp_err_to_name(err));
            return;
        }

        s_uart_up = true;
    }

    (void)obd_uart_write((const uint8_t *)" ", 1);
    vTaskDelay(pdMS_TO_TICKS(OBD_PARK_STOP_MS));
    obd_uart_flush_input();
    (void)obd_uart_write((const uint8_t *)WORD, sizeof(WORD) - 1);

    const int64_t until = esp_timer_get_time() + (int64_t)OBD_PARK_WORD_MS * 1000;

    while (len < sizeof(resp) - 1 && esp_timer_get_time() < until &&
           strchr(resp, '>') == NULL)
    {
        int got = obd_uart_read((uint8_t *)resp + len, sizeof(resp) - 1 - len, 20);

        if (got > 0)
        {
            len += (size_t)got;
            resp[len] = '\0';
        }
    }

    if (strstr(resp, "OK") != NULL)
    {
        ESP_LOGI(TAG, "park: chip asleep by command (STSLEEP0 answered OK)");
    }
    else
    {
        ESP_LOGW(TAG, "park: the chip did not take STSLEEP0 (%u byte(s)): the "
                 "pin alone", (unsigned)len);
    }
}

esp_err_t obd_chip_park(void)
{
    /* Pins only: nothing else of the component exists in a boot that
       parks. The chip meets a boot in one of two states (meatpi
       2026-10-05): awake, after a power-on or when the run before had it
       up; or asleep, when the boot follows a sleep and nobody woke it yet
       (the sleep pin's hold outlives the reset on purpose). */
    obd_pins_init();
    vTaskDelay(pdMS_TO_TICKS(5)); /* the READY input, before it is read */

    if (!obd_pin_ready())
    {
        /* asleep: leave it there. The sleep pin is confirmed low without
           ever being driven high (level before direction); under the hold
           of the sleep before, none of this even reaches the pad. */
        gpio_set_level(OBD_SLEEP_PIN, 0);
        gpio_set_direction(OBD_SLEEP_PIN, GPIO_MODE_OUTPUT);
        obd_pin_sleep();
        return ESP_OK;
    }

    /* awake, maybe mid-session or mid-monitor, and then it ignores the
       sleep pin. Until 2026-11-12 a pulse on the reset line came first; an
       awake chip gets none any more (issue #1023: the reset line is for a
       chip that does not answer). It is put to sleep as a normal sleep
       entry does it, by the sleep word and the pin, with a stop byte ahead
       for the monitor. A session left open ends with the sleep: the chip
       restarts itself at its next wake. The pin goes low only when READY
       has rested, so that no boot of the chip is cut short. */
    obd_pin_wake(); /* hold off, the sleep pin an output, high */
    (void)obd_pin_settle(OBD_PULSE_SETTLE_MS, NULL);
    park_sleep_word();
    obd_pin_sleep();
    return ESP_OK;
}
