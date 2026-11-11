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
 * @file autopid_bus.c
 * @brief The chip half of a row's bus (autopid_bus.h): taking the OBD chip
 *        to MS, single-wire or CH CAN for the rows that ask there, and home
 *        again. The runner's third file (autopid_runner_private.h).
 *
 * The chip's state the runner keeps (ap_base_mem_t) says where the chip is:
 * `bus` (AP_BUS_OBD while the baseline's protocol is pinned: every prelude
 * of the firmware puts it there), since when, and under which settings
 * generation of the chip it was taken there. A slot definition lives in the
 * chip's RAM, so anything that may have reset the chip or moved it behind
 * our back (a new generation, ap_runner_reset()) makes the next row of
 * another bus take the chip there again, from the car's protocol: a slot
 * must not be defined while the chip is on it (the requests behind it read
 * NO DATA, bench 2026-11-11).
 *
 * Poller-task context, plus the httpd task for test-a-PID while the poller
 * is paused (the chip job serializes them).
 */
#include "autopid_bus.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "autopid_runner_private.h"

static const char *TAG = "autopid";

#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

static int s_last_pid = -1;      /* the row of another bus whose init is in effect */
static uint8_t s_defined;        /* the buses whose slot this excursion defined    */
static ap_bus_err_t s_err[AP_BUS_COUNT]; /* CAN ERROR per bus, and its rest       */
static bool s_refused_logged;    /* one W per streak of refused slots              */
static ap_bus_stats_t s_stats;

uint16_t ap_bus_now(void)
{
    return ap_base_mem()->bus;
}

int64_t ap_bus_since_us(void)
{
    return ap_base_mem()->bus_since_us;
}

void ap_bus_note(uint8_t bus, bool ok)
{
    if (bus < AP_BUS_COUNT)
    {
        if (ok)
        {
            s_stats.ok[bus]++;
        }
        else
        {
            s_stats.failed[bus]++;
        }
    }
}

void ap_bus_get_stats(ap_bus_stats_t *out)
{
    int64_t now = esp_timer_get_time();

    *out = s_stats;

    for (uint8_t b = 0; b < AP_BUS_COUNT; b++)
    {
        out->parks[b] = s_err[b].parks;
        out->parked[b] = ap_bus_err_parked(&s_err[b], now);
    }
}

void ap_bus_reply(uint8_t bus, const char *resp)
{
    if (bus == AP_BUS_OBD || bus >= AP_BUS_COUNT || resp == NULL)
    {
        return;
    }

    if (ap_bus_err_note(&s_err[bus], strstr(resp, "CAN ERROR") != NULL,
                        esp_timer_get_time()))
    {
        ESP_LOGW(TAG, "%s: %d requests in a row ended in CAN ERROR (nobody "
                      "acknowledged: another speed than the car's, or no "
                      "such bus on this car). Its rows rest for %lu s",
                 ap_bus_label(bus), AP_BUS_ERR_STREAK,
                 (unsigned long)(s_err[bus].park_us / 1000000));
    }
}

static void bus_set(uint16_t bus)
{
    ap_base_mem_t *mem = ap_base_mem();

    if (bus == 0 || mem->bus == 0)
    {
        /* home again, or leaving home: the start the scheduler counts
           from. From one other bus to the next the excursion goes on */
        mem->bus_since_us = esp_timer_get_time();
    }

    mem->bus = bus;
    mem->bus_gen = ap_base_chip_gen();
    mem->bus_stale = false;
}

/** The protocol word alone: it sends no frame, so it needs no verdict of
 *  the bus guard. */
static void pin_base_proto(ap_tr_t *tr)
{
    char cmd[] = { 'A', 'T', 'T', 'P', ap_base_proto(), '\0' };
    char resp[64];

    (void)ap_base_request_tr(cmd, resp, sizeof(resp), AP_INIT_TIMEOUT, tr);
}

/**
 * The chip onto @p bus: what the tables moved put back, then the bus's
 * chain one word at a time. The slot choice is the one word that must be
 * acknowledged: a row that went out behind a refused `ATTP` would be asked
 * on the bus the chip was on before.
 */
static bool bus_enter(uint16_t bus, ap_tr_t *tr)
{
    ap_base_mem_t *mem = ap_base_mem();
    char chain[AP_INIT_LEN];
    char cmd[AP_INIT_LEN];
    char resp[64] = "";
    size_t n = 0;
    bool taken = false;

    if (ap_bus_chain(bus, chain, sizeof(chain)) == 0)
    {
        return false;
    }

    if (AP_BUS_OF(mem->bus) == AP_BUS_OF(bus) || mem->bus_stale)
    {
        /* the chip may sit on this very slot (the same bus at another
           speed, a reset of ours, another holder): off it first, a slot is
           not defined while it is chosen */
        pin_base_proto(tr);
    }

    if (mem->moved != 0)
    {
        char undo[AP_RESTORE_LEN];

        (void)ap_chain_restore(mem->moved, "", undo, sizeof(undo));
        ap_base_send_chain(undo, tr);
        mem->moved = 0;
    }

    for (const char *p = chain;; p++)
    {
        if (*p != ';' && *p != '\0')
        {
            if (n < sizeof(cmd) - 1)
            {
                cmd[n++] = *p;
            }

            continue;
        }

        cmd[n] = '\0';

        if (n > 0)
        {
            esp_err_t err = ap_base_request_tr(cmd, resp, sizeof(resp),
                                               AP_INIT_TIMEOUT, tr);

            if (strncmp(cmd, "ATTP", 4) == 0)
            {
                taken = (err == ESP_OK && strstr(resp, "OK") != NULL);

                if (!taken)
                {
                    break;
                }
            }
        }

        n = 0;

        if (*p == '\0')
        {
            break;
        }
    }

    if (!taken)
    {
        if (!s_refused_logged)
        {
            s_refused_logged = true;
            ESP_LOGW(TAG, "%s: the chip did not take slot %c (%s): its rows "
                          "are not asked", ap_bus_label(AP_BUS_OF(bus)),
                     ap_bus_slot(AP_BUS_OF(bus)),
                     (resp[0] != '\0') ? resp : "no reply");
        }

        /* the format words and the definition went out, the choice did not:
           where the chip is now is not ours to say */
        mem->bus_stale = true;
        return false;
    }

    s_refused_logged = false;
    s_defined |= (uint8_t)(1u << AP_BUS_OF(bus));
    mem->hdr_borrowed = false;
    mem->rxheader_set = false;  /* the chain's ATCRA cleared the filter */
    mem->init_pin = '\0';
    bus_set(bus);
    s_stats.changes++;
    s_last_pid = -1;
    ESP_LOGD(TAG, "the chip is on %s at %lu bit/s (slot %c)",
             ap_bus_label(AP_BUS_OF(bus)),
             (unsigned long)ap_bus_rate_bps(AP_BUS_OF(bus),
                                            AP_BUS_RATE_OF(bus)),
             ap_bus_slot(AP_BUS_OF(bus)));
    return true;
}

bool ap_bus_row_setup(const char *name, uint16_t bus, int pid_index,
                      const char *init, const char *rxheader, ap_tr_t *tr)
{
    ap_base_mem_t *mem = ap_base_mem();
    char resp[64];

    if (pid_index >= 0 &&
        ap_bus_err_parked(&s_err[AP_BUS_OF(bus) % AP_BUS_COUNT],
                          esp_timer_get_time()))
    {
        return false; /* the bus's rows rest (CAN ERRORs): nothing is sent */
    }

    if (mem->bus != bus || mem->bus_stale ||
        mem->bus_gen != ap_base_chip_gen())
    {
        if (!bus_enter(bus, tr))
        {
            ESP_LOGD(TAG, "%s: not sent (%s was not reached)",
                     (name != NULL) ? name : "the tested row",
                     ap_bus_label(AP_BUS_OF(bus)));
            return false;
        }
    }

    if (pid_index < 0 || pid_index != s_last_pid)
    {
        if (init != NULL && init[0] != '\0')
        {
            ap_base_send_init(init, tr);
        }

        if (rxheader != NULL && rxheader[0] != '\0')
        {
            char cra[AP_HDR_LEN + 8];

            snprintf(cra, sizeof(cra), "ATCRA%s", rxheader);
            (void)ap_base_request_tr(cra, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                     tr);
            mem->rxheader_set = true;
        }
        else if (mem->rxheader_set)
        {
            (void)ap_base_request_tr("ATCRA", resp, sizeof(resp),
                                     AP_INIT_TIMEOUT, tr);
            mem->rxheader_set = false;
        }

        s_last_pid = pid_index;
    }

    return true;
}

void ap_bus_leave(ap_tr_t *tr)
{
    ap_base_mem_t *mem = ap_base_mem();
    bool away = (mem->bus != 0);

    if (!away && s_defined == 0)
    {
        return;
    }

    uint8_t was = AP_BUS_OF(mem->bus);

    if (!away)
    {
        /* a prelude of somebody else's took the chip home (a scan, the
           contact probe): only the slots are owed */
    }
    else if (ap_base_restore(tr))
    {
        /* the baseline prelude went out (it pins the car's protocol and
           undoes what the rows of that bus set): in effect again */
        mem->baseline_sent = true;
        mem->chip_gen = ap_base_chip_gen();
    }
    else
    {
        /* no verdict of the bus guard for the car's protocol right now:
           off the other bus all the same, by the protocol word alone, and
           the baseline is due before the next row of the car's protocol */
        pin_base_proto(tr);
        mem->baseline_sent = false;
    }

    /* the slots as found: a definition lives in the chip's RAM until a
       reset, and the one-argument form takes it back. The chip is off them
       now, so it is safe to. Whoever is next (an app that uses a user
       protocol, the next snapshot of the chip) finds what was there */
    for (uint8_t b = AP_BUS_MS; b < AP_BUS_COUNT; b++)
    {
        if ((s_defined & (1u << b)) != 0)
        {
            char cmd[] = { 'V', 'T', 'S', 'E', 'T', '_', 'C', 'A', 'N', ' ',
                           ap_bus_slot(b), 0 };
            char resp[64];

            (void)ap_base_request_tr(cmd, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                     tr);
        }
    }

    s_defined = 0;

    if (!away)
    {
        return;
    }

    bus_set(0);
    s_stats.changes++;
    s_last_pid = -1;
    ap_runner_reset(); /* the type and row inits are due again */
    mem->bus_stale = false; /* ap_runner_reset() marks it: we know where it is */
    ESP_LOGD(TAG, "the chip is back from %s", ap_bus_label(was));
}
