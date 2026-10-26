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
 * @file autopid_scan_probe.c
 * @brief The detection job's profile probe (2026-10-26, discussion #1018,
 *        the Quick Setup's "Which car is this?"): when nothing standard
 *        answers (no 0100, no 22F400, no VIN: a Nissan Leaf) and the wizard
 *        named the car's published profile, the job sends the profile's own
 *        first requests behind its init chain; the first positive reply
 *        confirms the car, its responders are the ECU set the car is keyed
 *        by, and the job stores the entry under the profile's name with the
 *        profile attached (autopid_std_scan.c). The probe is armed by the
 *        detect route (ap_scan_probe_set, httpd task) before the job
 *        starts and consumed once by the scan task.
 */
#include "autopid_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "obd_chip.h"

static const char *TAG = "autopid";

#define AP_PROBE_REQ_TIMEOUT pdMS_TO_TICKS(4000) /* a multi-frame reply    */
#define AP_PROBE_AT_TIMEOUT  pdMS_TO_TICKS(2000)

static ap_scan_probe_t s_probe EXT_RAM_BSS_ATTR; /* set: httpd task;
                                                    read: the scan task   */

void ap_scan_probe_set(const ap_scan_probe_t *p)
{
    if (p == NULL)
    {
        memset(&s_probe, 0, sizeof(s_probe));
        return;
    }

    s_probe = *p;
    s_probe.armed = true;
}

void ap_scan_probe_clear(void)
{
    memset(&s_probe, 0, sizeof(s_probe));
}

const ap_scan_probe_t *ap_scan_probe(void)
{
    return &s_probe;
}

static esp_err_t probe_request(const char *cmd, char *resp, size_t len,
                               TickType_t timeout)
{
    resp[0] = '\0';
    return obd_chip_request(cmd, resp, len, timeout);
}

/** A ';'-separated chain one command at a time (the driver's EEPROM
 *  guard rewrites the profile's ATSP into ATTP on the way). */
static void send_chain(const char *chain, char *resp, size_t len)
{
    const char *p = chain;

    while (*p != '\0')
    {
        const char *sep = strchr(p, ';');
        size_t n = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char one[AP_INIT_LEN];

        if (n > 0 && n < sizeof(one))
        {
            memcpy(one, p, n);
            one[n] = '\0';
            (void)probe_request(one, resp, len, AP_PROBE_AT_TIMEOUT);
        }

        p += n + ((sep != NULL) ? 1 : 0);
    }
}

/** The service byte of a request: "2101" -> 0x21, "22F4051" -> 0x22. */
static int cmd_service(const char *cmd)
{
    unsigned v = 0;

    if (cmd == NULL || strlen(cmd) < 2 || sscanf(cmd, "%2x", &v) != 1)
    {
        return -1;
    }

    return (int)v;
}

bool ap_scan_probe_run(char *resp, size_t resp_len, ap_veh_seen_t *seen,
                       char *answered, size_t answered_cap)
{
    /* scan task only: eight reassembled replies are too big for its stack */
    static ap_resp_ecu_t ecus[AP_RESP_ECUS_MAX] EXT_RAM_BSS_ATTR;
    const ap_scan_probe_t *p = &s_probe;
    char proto = (p->protocol != '\0') ? p->protocol : '6';

    if (answered != NULL && answered_cap > 0)
    {
        answered[0] = '\0';
    }

    if (!p->armed || p->n_cmds == 0 || resp == NULL || seen == NULL)
    {
        return false;
    }

    /* the CAN prelude for the profile's protocol (headers on, the frame of
       reference of the profile), then the profile's own init chain */
    send_chain(ap_veh_prelude_for(proto), resp, resp_len);

    if (p->init[0] != '\0')
    {
        send_chain(p->init, resp, resp_len);
    }

    for (int i = 0; i < p->n_cmds && i < AP_PROBE_CMDS_MAX; i++)
    {
        const char *cmd = p->cmds[i];
        int sid = cmd_service(cmd);

        if (sid < 0)
        {
            continue;
        }

        /* a request's own init (a profile that addresses one ECU per
           request: ATSH / ATCRA per row) goes out before it */
        if (p->cmd_inits[i][0] != '\0')
        {
            send_chain(p->cmd_inits[i], resp, resp_len);
        }

        if (probe_request(cmd, resp, resp_len, AP_PROBE_REQ_TIMEOUT) != ESP_OK)
        {
            continue;
        }

        int n = ap_resp_to_payloads(resp, ecus, AP_RESP_ECUS_MAX);
        bool positive = false;

        seen->n_ecus = 0;

        for (int k = 0; k < n; k++)
        {
            /* a positive reply echoes the service + 0x40; a 7F negative
               response or a wrong echo is no confirmation */
            if (ecus[k].len == 0 ||
                ecus[k].payload[0] != (uint8_t)(sid + 0x40))
            {
                continue;
            }

            ap_veh_ecu_t one = { ecus[k].header, 0 };

            positive = true;
            (void)ap_veh_ecus_union(seen->ecus, &seen->n_ecus,
                                    AP_VEH_ECUS_MAX, &one);
        }

        if (!positive)
        {
            continue;
        }

        seen->protocol[0] = proto;
        seen->protocol[1] = '\0';
        seen->dialect = (uint8_t)AP_DIALECT_OBD2;
        seen->confirmed_by = AP_VEH_BY_PROFILE;

        if (answered != NULL && answered_cap > 0)
        {
            snprintf(answered, answered_cap, "%s", cmd);
        }

        ESP_LOGI(TAG, "detection: nothing standard answered, the profile's "
                      "%s did (%u responder%s): %s confirmed", cmd,
                 (unsigned)seen->n_ecus, seen->n_ecus == 1 ? "" : "s",
                 p->profile[0] ? p->profile : "(no profile name)");
        return true;
    }

    return false;
}
