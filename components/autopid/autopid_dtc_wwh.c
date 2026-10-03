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
 * @file autopid_dtc_wwh.c
 * @brief DTC scan and clear, the WWH-OBD path (ISO 27145-3 / SAE J1979-2,
 *        TASK_j1939_wwh.md phase 3): the legislated trouble codes of a
 *        vehicle whose dialect is `uds`. `dtc_protocol` = obd means "the
 *        legislated path" and lands here by the car's dialect.
 *
 * The shape is the OBD-II path's (autopid_dtc_obd.c): functional requests
 * with headers on, every responding ECU parsed apart by its CAN id. With
 * no receive filter the chip gives each ECU its flow control, so several
 * multi-frame lists arrive whole (bench 2026-10-03, two ECUs).
 *
 *   22 F4 01           the lamp and the confirmed count of every ECU
 *   19 42 33 08 1E     confirmed codes   (stored)
 *   19 42 33 04 1E     pending codes
 *   19 55 33           codes with permanent status
 *   14 FF FF 33        clear the emissions group: everything or nothing,
 *                      as OBD-II service 04
 *
 * The codec is uds_manager's (uds_dtc.h, pure). No freeze frame in v1.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "uds_dtc.h"

#include "autopid_transport.h"
#include "autopid_private.h"
#include "autopid_dtc_engine.h"

static const char *TAG = "autopid";

_Static_assert(UDS_DTC_TEXT_LEN <= AP_DTC_CODE_LEN,
               "a WWH code must fit the report's code field");

/** The request bytes as the chip wants them (hex, no spaces). */
static void hex_cmd(const uint8_t *req, size_t n, char *out, size_t cap)
{
    size_t w = 0;

    out[0] = '\0';

    for (size_t i = 0; i < n && w + 3 <= cap; i++)
    {
        w += (size_t)snprintf(out + w, cap - w, "%02X", req[i]);
    }
}

/** `22F401` from every responder: the lamp (A7) and the confirmed count
 *  (A6..A0), the J1979 PID 01 word. @return true when one answered. */
static bool wwh_lamps(ap_dtc_report_t *r, char *resp, size_t resp_len)
{
    ap_resp_ecu_t *ecus = ap_dtc_obd_ecus();
    bool any = false;

    if (ap_be()->request("22F401", resp, resp_len, AP_DTC_REQ_TIMEOUT) !=
        ESP_OK)
    {
        return false;
    }

    int n_ecu = ap_resp_to_payloads(resp, ecus, AP_RESP_ECUS_MAX);

    for (int e = 0; e < n_ecu; e++)
    {
        const uint8_t *p = ecus[e].payload;

        if (ecus[e].len >= 4 && p[0] == 0x62 && p[1] == 0xF4 &&
            p[2] == 0x01)
        {
            ap_dtc_report_src(r, ecus[e].header, (p[3] & 0x80) != 0,
                              p[3] & 0x7F);
            any = true;
        }
    }

    return any;
}

int ap_dtc_wwh_codes(ap_dtc_kind_t kind, ap_dtc_report_t *r, char *resp,
                     size_t resp_len)
{
    ap_resp_ecu_t *ecus = ap_dtc_obd_ecus();
    uint8_t req[5];
    char cmd[12];
    size_t n;

    if (kind == AP_DTC_KIND_PERMANENT)
    {
        n = uds_wwh_req_permanent(UDS_WWH_FGID_EMISSIONS, req);
    }
    else
    {
        n = uds_wwh_req_by_mask(UDS_WWH_FGID_EMISSIONS,
                                (kind == AP_DTC_KIND_PENDING)
                                    ? UDS_DTC_STATUS_PENDING
                                    : UDS_DTC_STATUS_CONFIRMED,
                                UDS_WWH_SEVERITY_CLASSES, req);
    }

    hex_cmd(req, n, cmd, sizeof(cmd));

    if (ap_be()->request(cmd, resp, resp_len, AP_DTC_REQ_TIMEOUT) != ESP_OK)
    {
        return -1;
    }

    int n_ecu = ap_resp_to_payloads(resp, ecus, AP_RESP_ECUS_MAX);
    bool answered = false;

    for (int e = 0; e < n_ecu; e++)
    {
        uds_wwh_dtc_t recs[AP_DTC_MAX];
        uint8_t format = 0;
        int c = (kind == AP_DTC_KIND_PERMANENT)
            ? uds_wwh_parse_permanent(ecus[e].payload, ecus[e].len,
                                      UDS_WWH_FGID_EMISSIONS, &format, recs,
                                      AP_DTC_MAX)
            : uds_wwh_parse_by_mask(ecus[e].payload, ecus[e].len,
                                    UDS_WWH_FGID_EMISSIONS, &format, recs,
                                    AP_DTC_MAX);

        if (c < 0)
        {
            continue;   /* a negative response, or somebody else's line */
        }

        answered = true;

        for (int i = 0; i < c; i++)
        {
            char text[UDS_DTC_TEXT_LEN];

            uds_wwh_dtc_text(format, &recs[i], text);
            (void)ap_dtc_report_add(r, kind, text, ecus[e].header,
                                    recs[i].status, recs[i].severity);
        }
    }

    if (!answered)
    {
        return -1;
    }

    return (kind == AP_DTC_KIND_PERMANENT) ? r->n_permanent
           : (kind == AP_DTC_KIND_PENDING) ? r->n_pending
                                           : r->n_stored;
}

bool ap_dtc_wwh_scan(ap_dtc_report_t *r, char *resp, size_t resp_len)
{
    /* an ECU without PID 01 can still hold codes: either answer makes
       the scan a scan */
    bool lamps = wwh_lamps(r, resp, resp_len);
    int stored = ap_dtc_wwh_codes(AP_DTC_KIND_STORED, r, resp, resp_len);

    if (!lamps && stored < 0)
    {
        return false;
    }

    if (ap_dtc_cfg()->want_pending)
    {
        (void)ap_dtc_wwh_codes(AP_DTC_KIND_PENDING, r, resp, resp_len);
    }

    if (ap_dtc_cfg()->want_permanent)
    {
        (void)ap_dtc_wwh_codes(AP_DTC_KIND_PERMANENT, r, resp, resp_len);
    }

    snprintf(r->protocol, sizeof(r->protocol), "wwh");
    r->valid = true;
    return true;
}

bool ap_dtc_wwh_clear(char *resp, size_t resp_len)
{
    ap_resp_ecu_t *ecus = ap_dtc_obd_ecus();
    uint8_t req[4];
    char cmd[12];

    hex_cmd(req, uds_wwh_req_clear(UDS_WWH_FGID_EMISSIONS, req), cmd,
            sizeof(cmd));

    if (ap_be()->request(cmd, resp, resp_len, AP_DTC_REQ_TIMEOUT) != ESP_OK)
    {
        return false;
    }

    /* every ECU that answers must confirm; a refusal (7F 14 xx: the
       conditions are not correct, engine running) is not a clear */
    int n_ecu = ap_resp_to_payloads(resp, ecus, AP_RESP_ECUS_MAX);
    int confirmed = 0;

    for (int e = 0; e < n_ecu; e++)
    {
        if (uds_dtc_clear_ok(ecus[e].payload, ecus[e].len))
        {
            confirmed++;
        }
        else if (ecus[e].len >= 3 && ecus[e].payload[0] == 0x7F &&
                 ecus[e].payload[1] == 0x14)
        {
            ESP_LOGW(TAG, "dtc clear refused by %lX (NRC %02X)",
                     (unsigned long)ecus[e].header, ecus[e].payload[2]);
            return false;
        }
    }

    return confirmed > 0;
}
