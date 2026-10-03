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
 * @file autopid_dtc_j1939.c
 * @brief The trouble codes of a J1939 network in the DTC report
 *        (TASK_j1939_wwh.md phases 5 and 6): every controller's active codes
 *        (DM1, SAE J1939-73) as the listener last heard them, with its
 *        lamps; in active mode also the previously active codes (DM2, asked
 *        for at scan time) and the clear (DM11 for the active codes, DM3 for
 *        the previously active ones, each a Request the controller
 *        acknowledges).
 *
 * A code reads `SPN110-0` (suspect parameter number, failure mode). An
 * active code joins the report's `stored` list (active = confirmed in J1939
 * terms), a previously active one the `pending` list (the report has three
 * categories and J1939 two; the items say `dm:2`), both with the source
 * address and the occurrence count. The malfunction indicator lamp folds
 * into `mil` as a responder's lamp word does; the red stop, amber warning
 * and protect lamps travel beside it.
 *
 * Nothing is asked in listen mode: a DM1 is broadcast once a second by every
 * controller that has something to say. The clear is then refused (403):
 * the codes are heard, not owned.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "j1939.h"

#include "autopid_private.h"
#include "autopid_dtc_engine.h"

static const char *TAG = "autopid";

_Static_assert(J1939_DTC_TEXT_LEN <= AP_DTC_CODE_LEN,
               "a J1939 code must fit the report's code field");

#define DM_SOURCES_MAX   16     /* controllers looked at per scan        */
#define DM2_WAIT_MS      1500   /* answers to the DM2 request (a BAM of a
                                   long list takes about a second)       */
#define DM2_FRESH_MS     30000  /* a DM2 older than this was somebody
                                   else's question: not this scan's      */
#define CLEAR_WAIT_MS    1500   /* acknowledgments of DM11 / DM3         */
#define CLEAR_SETTLE_MS  1500   /* the next DM1 broadcast after a clear  */
#define POLL_MS          50

static uint8_t lamp_bits(const j1939_lamps_t *l)
{
    return (uint8_t)((l->mil ? AP_DTC_LAMP_MIL : 0) |
                     (l->rsl ? AP_DTC_LAMP_RSL : 0) |
                     (l->awl ? AP_DTC_LAMP_AWL : 0) |
                     (l->pl ? AP_DTC_LAMP_PL : 0));
}

/** The controllers heard (below the null address). */
static size_t controllers(j1939_source_t *src, size_t max)
{
    size_t n = j1939_sources(src, max);
    size_t kept = 0;

    for (size_t i = 0; i < n; i++)
    {
        if (src[i].sa < J1939_ADDR_NULL)
        {
            src[kept++] = src[i];
        }
    }

    return kept;
}

/** One controller's DM1 (active) or DM2 (previously active) into @p r.
 *  @return codes added, -1 when it sent none. */
static int fold_dm(ap_dtc_report_t *r, uint8_t sa, bool previous)
{
    static j1939_dtc_t codes[AP_DTC_MAX];   /* dtc job task only (one job
                                               at a time): off its stack  */
    j1939_lamps_t lamps;
    j1939_msg_t info;
    size_t count = 0;
    int total = 0;

    esp_err_t err = previous
                        ? j1939_dm2(sa, &lamps, codes, AP_DTC_MAX, &count, &info)
                        : j1939_dm1(sa, &lamps, codes, AP_DTC_MAX, &count, &info);

    if (err != ESP_OK || (previous && info.age_ms > DM2_FRESH_MS))
    {
        return -1;
    }

    if (!previous)
    {
        ap_dtc_report_src_j1939(r, sa, lamp_bits(&lamps),
                                (count > UINT8_MAX) ? UINT8_MAX
                                                    : (uint8_t)count);
    }

    size_t kept = (count < AP_DTC_MAX) ? count : AP_DTC_MAX;

    for (size_t k = 0; k < kept; k++)
    {
        char text[J1939_DTC_TEXT_LEN];

        j1939_dtc_text(codes[k].spn, codes[k].fmi, text, sizeof(text));

        if (ap_dtc_report_add_j1939(r, text, sa, codes[k].oc, previous))
        {
            total++;
        }
    }

    return total;
}

bool ap_dtc_j1939_scan(ap_dtc_report_t *r)
{
    j1939_source_t src[DM_SOURCES_MAX];
    size_t n = controllers(src, DM_SOURCES_MAX);
    bool any = false;
    bool asked = false;
    unsigned active = 0, previous = 0;

    /* active mode: ask everyone for the previously active codes first,
       they take a moment to arrive (nothing else can be asked for) */
    if (j1939_active() && j1939_request(J1939_PGN_DM2, J1939_ADDR_GLOBAL) ==
                              ESP_OK)
    {
        asked = true;
        vTaskDelay(pdMS_TO_TICKS(DM2_WAIT_MS));
    }

    for (size_t i = 0; i < n; i++)
    {
        int got = fold_dm(r, src[i].sa, false);

        if (got >= 0)
        {
            any = true;
            active += (unsigned)got;
        }

        if (asked)
        {
            got = fold_dm(r, src[i].sa, true);

            if (got >= 0)
            {
                any = true;
                previous += (unsigned)got;
            }
        }
    }

    if (!any)
    {
        return false;
    }

    if (r->protocol[0] == '\0')
    {
        snprintf(r->protocol, sizeof(r->protocol), "j1939");
    }

    r->valid = true;
    ESP_LOGI(TAG, "dtc scan: %u J1939 controller%s, %u active code%s%s, "
                  "lamps 0x%X", (unsigned)r->n_src, (r->n_src == 1) ? "" : "s",
             active, (active == 1) ? "" : "s",
             asked ? " (DM2 asked)" : "", (unsigned)r->lamps);

    if (asked)
    {
        ESP_LOGI(TAG, "dtc scan: %u previously active code%s (DM2)", previous,
                 (previous == 1) ? "" : "s");
    }

    return true;
}

/* ---- the clear (active mode) ------------------------------------------------------ */

/** Every active code on the network right now, for the clear condition. */
static uint8_t present_codes(char out[][AP_DTC_CODE_LEN])
{
    static j1939_dtc_t codes[AP_DTC_MAX];
    j1939_source_t src[DM_SOURCES_MAX];
    size_t n = controllers(src, DM_SOURCES_MAX);
    uint8_t total = 0;

    for (size_t i = 0; i < n && total < AP_DTC_MAX; i++)
    {
        size_t count = 0;

        if (j1939_dm1(src[i].sa, NULL, codes, AP_DTC_MAX, &count, NULL) !=
            ESP_OK)
        {
            continue;
        }

        for (size_t k = 0; k < count && k < AP_DTC_MAX && total < AP_DTC_MAX;
             k++)
        {
            char text[J1939_DTC_TEXT_LEN];
            bool dup = false;

            j1939_dtc_text(codes[k].spn, codes[k].fmi, text, sizeof(text));

            for (uint8_t d = 0; d < total && !dup; d++)
            {
                dup = (strcmp(out[d], text) == 0);
            }

            if (!dup)
            {
                snprintf(out[total++], AP_DTC_CODE_LEN, "%s", text);
            }
        }
    }

    return total;
}

/** Ask @p pgn (DM11 or DM3) of every controller, wait for the
 *  acknowledgments. @return controllers that acknowledged positively;
 *  @p refused gets those that did not (negative, or no answer). */
static unsigned ask_clear(uint32_t pgn, const j1939_source_t *src, size_t n,
                          unsigned *refused)
{
    unsigned acked = 0;

    *refused = 0;

    for (size_t i = 0; i < n; i++)
    {
        if (j1939_request(pgn, src[i].sa) != ESP_OK)
        {
            (*refused)++;
        }
    }

    /* every acknowledgment in, or the wait is over */
    for (unsigned waited = 0; waited < CLEAR_WAIT_MS; waited += POLL_MS)
    {
        bool pending = false;

        for (size_t i = 0; i < n; i++)
        {
            if (j1939_request_outcome(pgn, src[i].sa, NULL, NULL) ==
                J1939_REQ_PENDING)
            {
                pending = true;
            }
        }

        if (!pending)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    for (size_t i = 0; i < n; i++)
    {
        uint8_t control = 0xFF;
        j1939_req_outcome_t out = j1939_request_outcome(pgn, src[i].sa, NULL,
                                                        &control);

        if (out == J1939_REQ_ACKED)
        {
            acked++;
        }
        else if (out != J1939_REQ_NONE)
        {
            (*refused)++;
            ESP_LOGI(TAG, "dtc clear: controller %02X %s DM%u", src[i].sa,
                     (out == J1939_REQ_NACKED) ? j1939_ack_name(control)
                                               : "did not answer",
                     (pgn == J1939_PGN_DM11) ? 11u : 3u);
        }
    }

    return acked;
}

esp_err_t ap_dtc_j1939_clear(const char *codes, ap_dtc_clear_mode_t mode,
                             bool *cleared, uint8_t *before, uint8_t *after,
                             char *err, size_t err_len)
{
    /* the dtc clear runs one at a time (the caller's gate): off the stack */
    static char present[AP_DTC_MAX][AP_DTC_CODE_LEN] EXT_RAM_BSS_ATTR;
    j1939_source_t src[DM_SOURCES_MAX];
    j1939_status_t st;

    (void)j1939_status(&st);

    if (!st.active)
    {
        /* the codes are heard, not asked: clearing them is a request on
           the network, which only active mode may send */
        snprintf(err, err_len, "J1939: clearing needs mode active (j1939)");
        return ESP_ERR_NOT_ALLOWED;     /* 403 with this sentence         */
    }

    if (!j1939_active())
    {
        snprintf(err, err_len, "J1939: no address on the bus yet (%s)",
                 j1939_claim_state_name(st.claim));
        return ESP_ERR_INVALID_STATE;   /* 409: try again in a moment    */
    }

    size_t n = controllers(src, DM_SOURCES_MAX);
    uint8_t n_present = present_codes(present);

    if (before != NULL)
    {
        *before = n_present;
    }

    if (!ap_dtc_clear_allowed(present, n_present, codes, mode))
    {
        ESP_LOGI(TAG, "dtc clear: condition not met (%u active J1939 codes)",
                 n_present);
        ap_events_dtc_clear(true, false, n_present, n_present);
        return ESP_OK;                  /* cleared stays false            */
    }

    unsigned refused11 = 0, refused3 = 0;
    unsigned acked11 = ask_clear(J1939_PGN_DM11, src, n, &refused11);
    unsigned acked3 = ask_clear(J1939_PGN_DM3, src, n, &refused3);
    bool ok = (acked11 > 0);

    if (!ok)
    {
        snprintf(err, err_len, "DM11 not acknowledged by %u controller%s",
                 (unsigned)n, (n == 1) ? "" : "s");
    }
    else
    {
        /* the controllers broadcast their DM1 once a second: the next one
           shows what is left */
        vTaskDelay(pdMS_TO_TICKS(CLEAR_SETTLE_MS));

        uint8_t n_after = present_codes(present);

        if (after != NULL)
        {
            *after = n_after;
        }

        if (cleared != NULL)
        {
            *cleared = true;
        }

        /* the report follows the network: a fresh scan reads it */
        (void)ap_dtc_scan_start();
    }

    ap_events_dtc_clear(ok, ok, n_present, (ok && after != NULL) ? *after : 0);
    ESP_LOGI(TAG, "dtc clear (J1939): DM11 acknowledged by %u of %u, DM3 by "
                  "%u (%u refused), %s", acked11, (unsigned)n, acked3,
             refused11 + refused3, ok ? "CLEARED" : "failed");
    return ok ? ESP_OK : ESP_FAIL;
}
