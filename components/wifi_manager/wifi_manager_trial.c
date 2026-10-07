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
 * @file wifi_manager_trial.c
 * @brief The connection TRIAL, pure (2026-10-06, Quick Setup: "test the
 *        station connect before storing and rebooting"). A trial joins a
 *        network with credentials that are never saved, waits for the
 *        association, the handshake and an address, and reports connected
 *        or the exact failure. This file is the state machine alone: it
 *        takes the driver's events and a clock and answers with the one
 *        action the glue must perform (connect, or finish). No IDF, so it
 *        runs in the host suite (test_trial.c). wifi_manager_try.c is the
 *        glue; wifi_manager.c hands it the events.
 */
#include <string.h>

#include "wifi_manager_private.h"

/* the driver's disconnect reasons, by what they mean to a person typing a
   password (wifi_err_reason_t numbers: no esp_wifi.h here on purpose) */
#define REASON_AUTH_EXPIRE            2u
#define REASON_4WAY_HANDSHAKE_TIMEOUT 15u
#define REASON_NO_AP_FOUND            201u
#define REASON_AUTH_FAIL              202u
#define REASON_HANDSHAKE_TIMEOUT      204u

void wm_trial_init(wm_trial_t *t)
{
    memset(t, 0, sizeof(*t));
}

bool wm_trial_active(const wm_trial_t *t)
{
    return t->state == WM_TRIAL_LEAVING || t->state == WM_TRIAL_CONNECTING ||
           t->state == WM_TRIAL_ASSOCIATED;
}

wm_trial_result_t wm_trial_classify(uint8_t reason)
{
    switch (reason)
    {
        case REASON_AUTH_EXPIRE:
        case REASON_4WAY_HANDSHAKE_TIMEOUT:
        case REASON_AUTH_FAIL:
        case REASON_HANDSHAKE_TIMEOUT:
            /* what a wrong password produces on this hardware: 204 is the
               one the HIL suite sees (test_s5), 15 and 2 on other routers */
            return WM_TRIAL_PASSWORD;

        case REASON_NO_AP_FOUND:
            return WM_TRIAL_NOT_FOUND;

        default:
            return WM_TRIAL_REFUSED;
    }
}

const char *wm_trial_result_name(wm_trial_result_t r)
{
    switch (r)
    {
        case WM_TRIAL_CONNECTED: return "connected";
        case WM_TRIAL_PASSWORD:  return "password";
        case WM_TRIAL_NOT_FOUND: return "not_found";
        case WM_TRIAL_REFUSED:   return "refused";
        case WM_TRIAL_NO_IP:     return "no_ip";
        case WM_TRIAL_TIMEOUT:   return "timeout";
        default:                 return "none";
    }
}

const char *wm_trial_state_name(wm_trial_state_t s)
{
    switch (s)
    {
        case WM_TRIAL_LEAVING:
        case WM_TRIAL_CONNECTING:
        case WM_TRIAL_ASSOCIATED: return "running";
        case WM_TRIAL_DONE:       return "done";
        default:                  return "idle";
    }
}

static wm_trial_action_t finish(wm_trial_t *t, wm_trial_result_t r,
                                uint32_t now_ms)
{
    t->state = WM_TRIAL_DONE;
    t->result = r;
    t->t_done = now_ms;
    return WM_TRIAL_ACT_FINISH;
}

wm_trial_action_t wm_trial_begin(wm_trial_t *t, const char *ssid,
                                 const char *password, uint8_t hint,
                                 bool sta_busy, uint32_t now_ms)
{
    size_t sl = strlen(ssid), pl = strlen(password);

    if (wm_trial_active(t))
    {
        return WM_TRIAL_ACT_NONE; /* one at a time: the glue says 409 */
    }

    if (sl == 0 || sl >= WM_SSID_LEN || pl >= WM_PASS_LEN ||
        (pl != 0 && pl < 8))
    {
        return WM_TRIAL_ACT_NONE; /* the glue says 400 */
    }

    memset(t, 0, sizeof(*t));
    strcpy(t->ssid, ssid);
    strcpy(t->password, password);
    t->hint = (hint >= 1 && hint <= 13) ? hint : 0; /* 2.4 GHz, or all */
    t->t_start = now_ms;

    /* a station that is connected, or mid-attempt, must let go first: the
       trial's connect follows its disconnect event (the roam trial's
       LEAVING step). An idle station connects at once. */
    if (sta_busy)
    {
        t->state = WM_TRIAL_LEAVING;
        return WM_TRIAL_ACT_NONE;
    }

    t->state = WM_TRIAL_CONNECTING;
    return WM_TRIAL_ACT_CONNECT;
}

wm_trial_action_t wm_trial_on_disconnect(wm_trial_t *t, uint8_t reason,
                                         uint32_t now_ms)
{
    switch (t->state)
    {
        case WM_TRIAL_LEAVING:
            t->state = WM_TRIAL_CONNECTING;
            t->t_start = now_ms; /* the budget counts from OUR attempt */
            return WM_TRIAL_ACT_CONNECT;

        case WM_TRIAL_CONNECTING:
        case WM_TRIAL_ASSOCIATED:
            t->reason = reason;
            /* the driver raises CONNECTED only once the handshake passed,
               so a wrong password never gets past CONNECTING; a drop after
               the association is the router letting go: the reason says */
            return finish(t, wm_trial_classify(reason), now_ms);

        default:
            return WM_TRIAL_ACT_NONE; /* not ours */
    }
}

void wm_trial_on_associated(wm_trial_t *t, uint8_t channel, uint32_t now_ms)
{
    if (t->state == WM_TRIAL_CONNECTING)
    {
        t->state = WM_TRIAL_ASSOCIATED;
        t->channel = channel;
        t->t_assoc = now_ms;
    }
}

wm_trial_action_t wm_trial_on_got_ip(wm_trial_t *t, const char *ip,
                                     int8_t rssi, uint32_t now_ms)
{
    if (t->state != WM_TRIAL_CONNECTING && t->state != WM_TRIAL_ASSOCIATED)
    {
        return WM_TRIAL_ACT_NONE;
    }

    strncpy(t->ip, ip, sizeof(t->ip) - 1);
    t->ip[sizeof(t->ip) - 1] = '\0';
    t->rssi = rssi;
    return finish(t, WM_TRIAL_CONNECTED, now_ms);
}

wm_trial_action_t wm_trial_tick(wm_trial_t *t, uint32_t now_ms)
{
    switch (t->state)
    {
        case WM_TRIAL_LEAVING:
            /* a disconnect event that never came (the driver had nothing
               to drop): go anyway */
            if (now_ms - t->t_start >= WM_TRIAL_LEAVE_MS)
            {
                t->state = WM_TRIAL_CONNECTING;
                t->t_start = now_ms;
                return WM_TRIAL_ACT_CONNECT;
            }
            return WM_TRIAL_ACT_NONE;

        case WM_TRIAL_ASSOCIATED:
        case WM_TRIAL_CONNECTING:
            /* the overall budget first: an association that came late is
               still a timeout; then the address budget of the association */
            if (now_ms - t->t_start >= WM_TRIAL_TOTAL_MS)
            {
                return finish(t, WM_TRIAL_TIMEOUT, now_ms);
            }
            if (t->state == WM_TRIAL_ASSOCIATED &&
                now_ms - t->t_assoc >= WM_TRIAL_IP_MS)
            {
                return finish(t, WM_TRIAL_NO_IP, now_ms);
            }
            return WM_TRIAL_ACT_NONE;

        default:
            return WM_TRIAL_ACT_NONE;
    }
}

uint32_t wm_trial_took_ms(const wm_trial_t *t)
{
    return (t->state == WM_TRIAL_DONE) ? t->t_done - t->t_start : 0;
}
