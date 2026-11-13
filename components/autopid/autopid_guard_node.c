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
 * @file autopid_guard_node.c
 * @brief The bus guard and a CAN node of the unit's own that runs
 *        (can_manager enabled): is one up, what bit rate is it fixed at, and
 *        what is on the CAN pins while the OBD chip is quiet.
 *
 * Split out of autopid_guard.c 2026-11-13 (700-line rule). Such a node is
 * part of the bus: it acknowledges the chip's requests and reads them as
 * frames, takes the chip's talk at the other bit rate for traffic it cannot
 * read, and its link keeps either word. The guard therefore holds what the
 * node reports against the quiet (autopid_guard.c, probe_now), and the quiet
 * is heard on the RX line itself: the node's counters stop once the chip's
 * own unanswered request has tripped its storm throttle.
 *
 * Never touches flash (the poller's stack is PSRAM).
 */
#include "autopid_private.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "can_manager.h"

#define AP_GUARD_QUIET_MS 300   /* the chip quiet this long: what comes is
                                   not ours                                  */

static int64_t s_heard_us;      /* noise last heard with the chip quiet
                                   (0 = never); the poller and the jobs ask
                                   under the guard's lock, first contact
                                   from the poller's task                    */

bool ap_guard_node_runs(uint16_t *fixed_kbps)
{
    can_manager_status_t st;
    bool runs = can_manager_status(&st) == ESP_OK && st.running;

    if (fixed_kbps != NULL)
    {
        /* a node on "auto" tries every bit rate of the setting: none of them
           is the one it could not read the bus with */
        *fixed_kbps = (runs && !st.baud_auto) ? (uint16_t)st.baud_kbps : 0;
    }

    return runs;
}

ap_heard_t ap_guard_node_heard(void)
{
    can_manager_status_t a, b;
    bool line = false;

    if (can_manager_status(&a) != ESP_OK)
    {
        return AP_HEARD_NOISE; /* cannot tell: nothing is explained away */
    }

    /* not the counters alone: a node whose interrupts the storm throttle
       masked counts nothing, and the chip talked into 200 frames a second
       for 9 s (bench 2026-11-13) */
    if (can_manager_line_heard(AP_GUARD_QUIET_MS, &line) != ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(AP_GUARD_QUIET_MS));
    }

    int64_t now = esp_timer_get_time();

    if (can_manager_status(&b) != ESP_OK)
    {
        return AP_HEARD_NOISE;
    }

    if (b.stats.rx_count != a.stats.rx_count)
    {
        return AP_HEARD_FRAMES;
    }

    if (line || b.stats.rx_bad != a.stats.rx_bad ||
        b.stats.rx_deaf != a.stats.rx_deaf)
    {
        s_heard_us = now;
    }

    /* noise stays heard for a while: thin traffic misses a look */
    return ap_guard_heard_stands(now, s_heard_us) ? AP_HEARD_NOISE
                                                  : AP_HEARD_NOTHING;
}
