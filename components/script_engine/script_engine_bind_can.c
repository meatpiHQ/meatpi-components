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
 * @file script_engine_bind_can.c
 * @brief The bindings that reach the CAN controller itself: one raw frame
 *        received, the bus stopped and started, CAN_TX driven by hand.
 *        From pull request 2 (Elias Kotlyar), moved out of
 *        script_engine_bind.c as they were, to keep that file from growing.
 *
 *   can_rx(id, timeout_ms[, ext])  -> the frame's bytes as hex, or nil;
 *                                     sets can_rx_id / can_rx_ext / can_rx_dlc
 *   can_send(bus, id, hexbytes[, wake[, ext]]) -> bool; sets can_send_error
 *                                     (any bus of the OBD connector: autopid's
 *                                     can.send, 2026-11-21, issue #1072)
 *   can_start() / can_stop()       -> bool
 *   digitalWrite(pin, value)       -> nil (the pin rule: script_engine_gpio.c)
 *   digitalPWM(pin, on_ms, off_ms[, rounds[, idle]]) -> nil
 *
 * se_script_epilogue() runs after EVERY run: it drops a can_rx subscription,
 * hands the pins back and starts the bus again if the script stopped it.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "berry.h"
#include "cJSON.h"

#include "autopid.h"
#include "can_manager.h"

#include "script_engine_doc.h"
#include "script_engine_gpio.h"
#include "script_engine_flash.h"
#include "script_engine_private.h"

/* Set when this run called can_stop(); the epilogue brings the bus back. */
static bool s_bus_stopped;

static QueueHandle_t s_rx_q;
static int s_rx_idx = -1;

static void can_rx_drop(void)
{
    if (s_rx_idx >= 0)
    {
        (void)can_manager_unsubscribe_queue(s_rx_idx);
        s_rx_idx = -1;
    }

    if (s_rx_q != NULL)
    {
        vQueueDelete(s_rx_q);
        s_rx_q = NULL;
    }
}

void se_script_epilogue(void)
{
    can_rx_drop();
    se_gpio_release_all();

    if (s_bus_stopped)
    {
        (void)can_manager_start();
        s_bus_stopped = false;
    }
}

int se_b_can_start(bvm *vm)
{
    se_check_budget(vm);
    se_refuse_when_locked(vm);

    esp_err_t err = can_manager_start();
    bool up = err == ESP_OK && can_manager_core_handle() != NULL;

    if (up)
    {
        s_bus_stopped = false;
    }

    be_pushbool(vm, up);
    be_return(vm);
}

int se_b_can_stop(bvm *vm)
{
    se_check_budget(vm);
    se_refuse_when_locked(vm); /* never the bus away from a flash */
    se_tp_bus_down();          /* flash sessions die with the bus */

    esp_err_t err = can_manager_stop();

    if (err == ESP_OK)
    {
        s_bus_stopped = true;
    }

    be_pushbool(vm, err == ESP_OK);
    be_return(vm);
}

static void set_int_global(bvm *vm, const char *name, bint v)
{
    be_pushint(vm, v);
    be_setglobal(vm, name);
    be_pop(vm, 1);
}

int se_b_can_rx(bvm *vm)
{
    se_check_budget(vm);

    if (be_top(vm) < 2 || !be_isint(vm, 2))
    {
        be_raise(vm, "can_error", "can_rx(id, timeout_ms[, ext])");
    }

    uint32_t id = se_bind_id_arg(vm, 1);
    bint ms = be_toint(vm, 2);
    bool ext = id > 0x7FF;

    if (be_top(vm) >= 3 && be_isint(vm, 3))
    {
        ext = be_toint(vm, 3) != 0;
    }

    if (ms < 0 || ms > SE_SLEEP_MAX_MS)
    {
        be_raise(vm, "can_error", "timeout_ms is 0..60000");
    }

    if (can_manager_core_handle() == NULL)
    {
        be_raise(vm, "can_error", "CAN bus is stopped");
    }

    can_rx_drop();
    s_rx_q = xQueueCreate(4, sizeof(can_core_frame_t));

    if (s_rx_q == NULL)
    {
        be_raise(vm, "can_error", "no memory");
    }

    uint32_t mask = ext ? 0x1FFFFFFFUL : 0x7FFUL;
    esp_err_t err = can_manager_subscribe_queue(s_rx_q, id, mask, ext,
                                                false, &s_rx_idx);

    if (err != ESP_OK)
    {
        can_rx_drop();
        be_raise(vm, "can_error", "CAN bus is stopped");
    }

    (void)can_manager_subscriber_name(s_rx_idx, "script");

    can_core_frame_t frame;
    bool got = false;
    TickType_t left = pdMS_TO_TICKS((uint32_t)ms);

    if (ms > 0 && left == 0)
    {
        left = 1;
    }

    do
    {
        TickType_t slice = left > pdMS_TO_TICKS(50) ? pdMS_TO_TICKS(50) : left;

        if (xQueueReceive(s_rx_q, &frame, slice) == pdTRUE)
        {
            got = true;
            break;
        }

        if (left <= slice)
        {
            break;
        }

        left -= slice;
        se_check_budget(vm);
    }
    while (left > 0);

    can_rx_drop();

    if (!got)
    {
        be_return_nil(vm);
    }

    char hex[8 * 3 + 1];
    size_t o = 0;

    for (uint8_t i = 0; i < frame.dlc && i < 8 && o + 3 < sizeof(hex); i++)
    {
        o += (size_t)snprintf(hex + o, sizeof(hex) - o,
                              i ? " %02X" : "%02X", frame.data[i]);
    }

    hex[o] = '\0';
    set_int_global(vm, "can_rx_id", (bint)frame.id);
    set_int_global(vm, "can_rx_ext", frame.ext ? 1 : 0);
    set_int_global(vm, "can_rx_dlc", frame.dlc);
    be_pushstring(vm, hex);
    be_return(vm);
}

static bool pin_from(bvm *vm, int idx, int *pin)
{
    if (be_isint(vm, idx))
    {
        *pin = (int)be_toint(vm, idx);
        return true;
    }

    if (be_isstring(vm, idx))
    {
        const char *s = be_tostring(vm, idx);

        if (strcmp(s, "CAN_TX") == 0)
        {
            *pin = se_can_tx_gpio();
            return true;
        }

        if (strcmp(s, "CAN_RX") == 0)
        {
            *pin = se_can_rx_gpio();
            return true;
        }
    }

    return false;
}

int se_b_digital_write(bvm *vm)
{
    se_check_budget(vm);

    int pin = 0;

    if (be_top(vm) < 2 || !pin_from(vm, 1, &pin) || !be_isint(vm, 2))
    {
        be_raise(vm, "gpio_error", "digitalWrite(pin, HIGH|LOW)");
    }

    const char *why = se_gpio_check(pin, can_manager_core_handle() != NULL);

    if (why != NULL)
    {
        be_raise(vm, "gpio_error", why);
    }

    se_gpio_touch(pin);
    if (pin == se_can_tx_gpio())
    {
        can_manager_transceiver(true); /* out of standby: the pin reaches the bus */
    }

    se_gpio_write(pin, be_toint(vm, 2) ? 1 : 0);
    be_return_nil(vm);
}

static bool berry_num(bvm *vm, int idx, double *out)
{
    if (be_isreal(vm, idx))
    {
        *out = be_toreal(vm, idx);
        return true;
    }

    if (be_isint(vm, idx))
    {
        *out = (double)be_toint(vm, idx);
        return true;
    }

    return false;
}

int se_b_digital_pwm(bvm *vm)
{
    se_check_budget(vm);

    int pin = 0;
    double on_ms = 0;
    double off_ms = 0;

    if (be_top(vm) < 3 || !pin_from(vm, 1, &pin) ||
        !berry_num(vm, 2, &on_ms) || !berry_num(vm, 3, &off_ms))
    {
        be_raise(vm, "gpio_error",
                 "digitalPWM(pin, ontime_ms, offtime_ms[, rounds[, idle]])");
    }

    int rounds = 1;

    if (be_top(vm) >= 4 && !be_isnil(vm, 4))
    {
        if (!be_isint(vm, 4))
        {
            be_raise(vm, "gpio_error", "rounds is an integer");
        }

        rounds = (int)be_toint(vm, 4);
    }

    int idle = se_pwm_default_idle(pin);

    if (be_top(vm) >= 5 && !be_isnil(vm, 5))
    {
        if (!be_isint(vm, 5))
        {
            be_raise(vm, "gpio_error", "idle is HIGH or LOW");
        }

        idle = be_toint(vm, 5) ? 1 : 0;
    }

    se_pwm_plan_t plan;
    const char *err = NULL;

    if (!se_pwm_plan(on_ms, off_ms, rounds, &plan, &err))
    {
        be_raise(vm, "gpio_error",
                 err != NULL && strcmp(err, "cap") == 0
                     ? "digitalPWM longer than 1 s"
                     : "digitalPWM times or rounds");
    }

    const char *why = se_gpio_check(pin, can_manager_core_handle() != NULL);

    if (why != NULL)
    {
        be_raise(vm, "gpio_error", why);
    }

    plan.idle = idle;
    se_gpio_touch(pin);

    if (pin == se_can_tx_gpio())
    {
        can_manager_transceiver(true);
    }

    if (!se_gpio_pwm(pin, &plan))
    {
        be_raise(vm, "gpio_error", "no RMT channel");
    }

    be_return_nil(vm);
}

static void set_str_global(bvm *vm, const char *name, const char *v)
{
    be_pushstring(vm, v);
    be_setglobal(vm, name);
    be_pop(vm, 1);
}

/** An optional flag argument: true, or an integer that is not 0. */
static bool flag_arg(bvm *vm, int idx)
{
    if (be_top(vm) < idx || be_isnil(vm, idx))
    {
        return false;
    }

    return be_isbool(vm, idx) ? be_tobool(vm, idx)
                              : (be_isint(vm, idx) && be_toint(vm, idx) != 0);
}

/* can_send(bus, id, hexbytes[, wake[, ext]]): one raw frame on any bus of
   the OBD connector, through autopid's can.send (the OBD chip for 'obd',
   'ms', 'sw' and 'ch', WiCAN's own controller for 'can'). true when the
   frame is on the bus (CAN: acknowledged). A request that is malformed
   raises; one that cannot be served right now, or a frame nobody
   acknowledged, returns false with the reason in can_send_error. */
int se_b_can_send(bvm *vm)
{
    se_check_budget(vm);
    se_refuse_when_locked(vm);

    if (be_top(vm) < 3 || !be_isstring(vm, 1) || !be_isstring(vm, 3) ||
        !(be_isint(vm, 2) || be_isstring(vm, 2)))
    {
        be_raise(vm, "can_error", "can_send(bus, id, hexbytes[, wake[, ext]])");
    }

    cJSON *req = cJSON_CreateObject();

    if (req == NULL)
    {
        be_raise(vm, "can_error", "no memory");
    }

    cJSON_AddStringToObject(req, "bus", be_tostring(vm, 1));
    cJSON_AddNumberToObject(req, "id", (double)se_bind_id_arg(vm, 2));
    cJSON_AddStringToObject(req, "data", be_tostring(vm, 3));

    if (flag_arg(vm, 4))
    {
        cJSON_AddBoolToObject(req, "wake", true);
    }

    if (be_top(vm) >= 5 && !be_isnil(vm, 5))
    {
        cJSON_AddBoolToObject(req, "ext", flag_arg(vm, 5));
    }

    char err[200];
    esp_err_t serr = autopid_can_send(req, NULL, err, sizeof(err));

    cJSON_Delete(req);

    if (serr == ESP_ERR_INVALID_ARG)
    {
        be_raise(vm, "can_error", err); /* Berry copies the text */
    }

    set_str_global(vm, "can_send_error", (serr == ESP_OK) ? "" : err);
    be_pushbool(vm, serr == ESP_OK);
    be_return(vm);
}

void se_bind_can_globals(bvm *vm)
{
    set_str_global(vm, "can_send_error", "");
    set_int_global(vm, "can_rx_id", 0);
    set_int_global(vm, "can_rx_ext", 0);
    set_int_global(vm, "can_rx_dlc", 0);
    set_int_global(vm, "HIGH", 1);
    set_int_global(vm, "LOW", 0);
    set_int_global(vm, "CAN_TX", se_can_tx_gpio());
    set_int_global(vm, "CAN_RX", se_can_rx_gpio());
}
