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
 * @file autopid_dtc_engine.h
 * @brief What the DTC engine files share (autopid_dtc.c settings + scan +
 *        clear, autopid_dtc_job.c, autopid_dtc_obd.c, autopid_dtc_uds.c,
 *        autopid_dtc_report.c). Private to those files: everybody else
 *        goes through autopid_private.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "uds_manager.h"

#include "autopid_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/* first request after a protocol prelude can sit in SEARCHING — the std
 * scan learned the same lesson (AP_SCAN_REQ_TIMEOUT 10 s) */
#define AP_DTC_REQ_TIMEOUT   pdMS_TO_TICKS(5000)
#define AP_DTC_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

/* UDS path (TASK_dtc §12) */
typedef enum
{
    AP_DTC_PROTO_OBD = 0,
    AP_DTC_PROTO_UDS,
    AP_DTC_PROTO_AUTO,          /* OBD first, UDS when no ECU answers    */
} ap_dtc_proto_t;

/** The settings-applied knobs (written by ap_dtc_apply_settings only). */
typedef struct
{
    bool           enabled;
    bool           allow_clear;
    uint32_t       period_min;
    bool           want_pending;
    bool           want_permanent;
    bool           want_freeze;
    char           init[AP_INIT_LEN];
    char           rxheader[AP_HDR_LEN];
    ap_dtc_proto_t proto;
    uds_addr_t     uds_addr;
    uint8_t        uds_mask;
} ap_dtc_cfg_t;

const ap_dtc_cfg_t *ap_dtc_cfg(void);

/* ---- the OBD path (autopid_dtc_obd.c): functional, through the chip ---- */

/** dtc_init commands, headers on, ATCRA rxheader. Best-effort. */
void ap_dtc_obd_prep(char *resp, size_t resp_len);
void ap_dtc_obd_done(char *resp, size_t resp_len);

/** The per-ECU assembly buffer of the last OBD request (one job at a
 *  time): AP_RESP_ECUS_MAX entries. */
ap_resp_ecu_t *ap_dtc_obd_ecus(void);

/** Request @p cmd, parse its DTC list from every responding ECU: merged
 *  into @p out (may be NULL), and into @p r (may be NULL) as codes of
 *  @p kind with the ECU that reported each.
 *  @return the merged count in @p out (0 without it: read @p r). */
uint8_t ap_dtc_obd_codes(const char *cmd, uint8_t svc,
                         char out[][AP_DTC_CODE_LEN], ap_dtc_report_t *r,
                         ap_dtc_kind_t kind, char *resp, size_t resp_len);

/** Mode 02 frame 0 into @p r. Best-effort. */
void ap_dtc_obd_freeze(ap_dtc_report_t *r, char *resp, size_t resp_len);

/* ---- the UDS path (autopid_dtc_uds.c): one address pair, uds_request ---- */

/** One `19 02 <mask>`: the codes, and their status bytes into @p status
 *  (AP_DTC_MAX entries, may be NULL). */
int  ap_dtc_uds_codes(uint8_t mask, char out[][AP_DTC_CODE_LEN],
                      uint8_t *status);
bool ap_dtc_uds_clear(const char *codes);
bool ap_dtc_uds_scan(ap_dtc_report_t *r);

/* ---- the WWH-OBD path (autopid_dtc_wwh.c): the legislated codes of a
 *      UDS-dialect vehicle, functional with headers on like the OBD path ---- */

/** Lamp words, confirmed, pending and permanent codes into @p r.
 *  @return true when an ECU answered. */
bool ap_dtc_wwh_scan(ap_dtc_report_t *r, char *resp, size_t resp_len);

/** One category from every ECU into @p r. @return the category's count
 *  afterwards, -1 when no ECU answered the request. */
int  ap_dtc_wwh_codes(ap_dtc_kind_t kind, ap_dtc_report_t *r, char *resp,
                      size_t resp_len);

/** `14 FF FF 33`: true when every answering ECU confirmed (at least one). */
bool ap_dtc_wwh_clear(char *resp, size_t resp_len);

/* the J1939 network's codes (autopid_dtc_j1939.c): every controller's
   DM1 from the listener's store; in active mode DM2 is asked for first
   (the previously active codes, as pending items). @return true when at
   least one controller had sent a DM1 or DM2 (the report is valid then).
   Blocks up to 1.5 s in active mode: job-task context. */
bool ap_dtc_j1939_scan(ap_dtc_report_t *r);

/** The clear on a J1939 network (active mode only): DM11 to every
 *  controller heard, then DM3; cleared when at least one controller
 *  acknowledged DM11; a fresh scan is queued afterwards. Out-parameters and
 *  errors as ap_dtc_clear: ESP_ERR_NOT_ALLOWED (403) in listen mode,
 *  ESP_ERR_INVALID_STATE (409) while no address is held, ESP_FAIL when
 *  nobody acknowledged. Seconds of waiting: never the event dispatcher. */
esp_err_t ap_dtc_j1939_clear(const char *codes, ap_dtc_clear_mode_t mode,
                             bool *cleared, uint8_t *before, uint8_t *after,
                             char *err, size_t err_len);

/* ---- the report, read in place (autopid_dtc.c owns it and its lock) ---- */

const ap_dtc_report_t *ap_dtc_report_lock(void);
void ap_dtc_report_unlock(void);

/* ---- engine -> job task (autopid_dtc_job.c) ---- */

/** One scan; the caller paused the poller and holds the job flag. */
void ap_dtc_run_scan(char *resp, size_t resp_len);

#ifdef __cplusplus
}
#endif
