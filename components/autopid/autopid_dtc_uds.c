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
 * @file autopid_dtc_uds.c
 * @brief DTC scan, the UDS path (TASK_dtc.md §12): 19 02 / 14 to ONE
 *        physical address pair through uds_request() (native ISO-TP when
 *        can_manager runs, the chip otherwise). Split out of
 *        autopid_dtc.c 2026-10-02 (700-line rule); behaviour unchanged.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"

#include "uds_dtc.h"
#include "uds_manager.h"

#include "autopid_private.h"
#include "autopid_dtc_engine.h"

/* ---- the UDS path (TASK_dtc §12) --------------------------------------------------- */

/** One `19 02 <mask>` transaction -> formatted codes ("P0420" /
 *  "P0420-08"). @return count, or -1 = no usable answer (transport
 *  down / timeout / negative response). */
int ap_dtc_uds_codes(uint8_t mask, char out[][AP_DTC_CODE_LEN],
                     uint8_t *status)
{
    static uint8_t resp[8 + AP_DTC_MAX * 4] EXT_RAM_BSS_ATTR; /* job    */
    uint8_t req[3];
    size_t rlen = 0;
    uds_result_t res;
    size_t n = uds_dtc_req_by_status(mask, req);

    if (uds_request(&ap_dtc_cfg()->uds_addr, req, n, resp, sizeof(resp), &rlen,
                    NULL, &res) != ESP_OK || res.negative)
    {
        return -1;
    }

    uds_dtc_t recs[AP_DTC_MAX];
    int cnt = uds_dtc_parse_list(resp, rlen, NULL, recs, AP_DTC_MAX);

    if (cnt < 0)
    {
        return -1;
    }

    for (int i = 0; i < cnt; i++)
    {
        uds_dtc_format(recs[i].hi, recs[i].mid, recs[i].ftb, out[i]);

        if (status != NULL)
        {
            status[i] = recs[i].status;
        }
    }

    return cnt;
}

/** One 14 <group> transaction. NULL group = FFFFFF (all). */
static bool dtc_uds_clear_group(const uint8_t group[3])
{
    uint8_t req[4];
    uint8_t resp[8];
    size_t rlen = 0;
    uds_result_t res;
    size_t n = uds_dtc_req_clear(group, req);

    return uds_request(&ap_dtc_cfg()->uds_addr, req, n, resp, sizeof(resp), &rlen,
                       NULL, &res) == ESP_OK &&
           !res.negative && uds_dtc_clear_ok(resp, rlen);
}

/** Clear via UDS: a CSV codes list -> one 14 per code (TRUE selective
 *  clear, the thing OBD mode 04 can't do); empty list -> group
 *  FFFFFF. ALL calls must confirm. */
bool ap_dtc_uds_clear(const char *codes)
{
    if (codes == NULL || codes[0] == '\0')
    {
        return dtc_uds_clear_group(NULL);
    }

    const char *p = codes;

    while (*p != '\0')
    {
        while (*p == ' ' || *p == ',')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        char one[AP_DTC_CODE_LEN];
        size_t n = 0;

        while (p[n] != '\0' && p[n] != ',' && p[n] != ' ' &&
               n < sizeof(one) - 1)
        {
            one[n] = p[n];
            n++;
        }

        one[n] = '\0';
        p += n;

        uint8_t group[3];

        if (!uds_dtc_unformat(one, &group[0], &group[1], &group[2]) ||
            !dtc_uds_clear_group(group))
        {
            return false;
        }
    }

    return true;
}

/** UDS scan: stored = 19 02 <mask>, pending = 19 02 pendingDTC-bit.
 *  Permanent has no UDS equivalent (OBD-only concept) and MIL is not
 *  carried by 0x19: both stay zero under forced `uds` (documented,
 *  TASK_dtc §12). @return true when the ECU answered. */
bool ap_dtc_uds_scan(ap_dtc_report_t *r)
{
    /* the job task only: half a kilobyte that need not be its stack */
    static char codes[AP_DTC_MAX][AP_DTC_CODE_LEN] EXT_RAM_BSS_ATTR;
    uint8_t status[AP_DTC_MAX];
    uint32_t ecu = ap_dtc_cfg()->uds_addr.rx_id;
    int n = ap_dtc_uds_codes(ap_dtc_cfg()->uds_mask, codes, status);

    if (n < 0)
    {
        return false;
    }

    for (int i = 0; i < n; i++)
    {
        (void)ap_dtc_report_add(r, AP_DTC_KIND_STORED, codes[i], ecu,
                                status[i], 0);
    }

    if (ap_dtc_cfg()->want_pending)
    {
        n = ap_dtc_uds_codes(UDS_DTC_STATUS_PENDING, codes, status);

        for (int i = 0; i < n; i++)
        {
            (void)ap_dtc_report_add(r, AP_DTC_KIND_PENDING, codes[i], ecu,
                                    status[i], 0);
        }
    }

    r->n_ecus = 1;              /* one address pair in v1                */
    snprintf(r->protocol, sizeof(r->protocol), "uds");
    return true;
}
