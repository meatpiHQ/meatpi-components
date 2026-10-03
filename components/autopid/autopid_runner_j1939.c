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
 * @file autopid_runner_j1939.c
 * @brief The passive runner: a `PGN:` row served from the J1939 listener's
 *        store (TASK_j1939_wwh.md phase 5). Poller-task context.
 *
 * Nothing here touches the OBD chip or the bus: the row's parameters are
 * evaluated over the newest message of its group, exactly as a chip row's
 * are over the chip's reply (ap_runner_publish), and only when the store
 * holds a message the row has not published yet. The scheduler looks at a
 * max-rate row every AP_PASSIVE_FLOOR_MS; a row whose group nobody sends
 * fails like a silent PID and backs off the same way.
 *
 * A message of ANOTHER source that is already older than AP_J1939_STALE_MS
 * when a row first sees it is not news: the store's pick moved (a bus that
 * fell silent leaves the lowest source as the pick, but a row may have been
 * following another source that spoke last). Values are stamped with the
 * message's own time, so their age is the data's age.
 *
 * A `?` row (an on-request group) asks for its group before the look when
 * the poller allows it (no diagnostics hold) and the j1939 component holds
 * an address (active mode): the answer is in the store at the next look,
 * one period later. A controller that answered a request negatively fails
 * the row for AP_J1939_NACK_HOLD_MS without asking again (the scheduler's
 * backoff does the rest); in listen mode the row only reads.
 *
 * The message buffer is one PSRAM static (a transport-protocol message is
 * up to 1785 bytes); the poller task is its only user.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "j1939.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

/* what each row published last: the message is new when its key or its
   count moved (the pick can move to another source without a count
   change, hence both) */
typedef struct
{
    uint32_t count;
    uint8_t  sa;
    bool     seen;
} row_mark_t;

static row_mark_t s_mark[AP_MAX_PIDS] EXT_RAM_BSS_ATTR;
static uint8_t    s_msg[J1939_MSG_MAX] EXT_RAM_BSS_ATTR;
static uint32_t   s_published;
static uint32_t   s_looked;
static uint32_t   s_requested;
static uint32_t   s_refused;

void ap_runner_j1939_reset(void)
{
    memset(s_mark, 0, sizeof(s_mark));
}

void ap_runner_j1939_stats(uint32_t *published, uint32_t *looked)
{
    if (published != NULL)
    {
        *published = s_published;
    }

    if (looked != NULL)
    {
        *looked = s_looked;
    }
}

void ap_runner_j1939_tx_stats(uint32_t *requested, uint32_t *refused)
{
    if (requested != NULL)
    {
        *requested = s_requested;
    }

    if (refused != NULL)
    {
        *refused = s_refused;
    }
}

/** The request of a `?` row. @return false when the controller refused the
 *  group recently: the look is over, the row fails. */
static bool ask(const ap_pid_t *pid, bool tx_ok)
{
    uint8_t da = (pid->j1939_sa >= 0) ? (uint8_t)pid->j1939_sa
                                      : J1939_ADDR_GLOBAL;
    uint32_t age_ms = 0;
    uint8_t control = 0xFF;

    if (!pid->j1939_request || !tx_ok || !j1939_active())
    {
        return true; /* a listener, or held: read what the store has */
    }

    if (j1939_request_outcome(pid->pgn, da, &age_ms, &control) ==
            J1939_REQ_NACKED && age_ms < AP_J1939_NACK_HOLD_MS)
    {
        ESP_LOGD(TAG, "%s: PGN %lX refused by %02X (%s), not asking again yet",
                 pid->name, (unsigned long)pid->pgn, da,
                 j1939_ack_name(control));
        s_refused++;
        return false;
    }

    if (j1939_request(pid->pgn, da) == ESP_OK)
    {
        s_requested++;
    }

    return true;
}

esp_err_t ap_runner_test_j1939(uint32_t pgn, int sa, uint8_t *payload,
                               size_t cap, size_t *len, char *transcript,
                               size_t tr_cap)
{
    j1939_msg_t info;
    j1939_status_t st;

    if (payload == NULL || len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *len = 0;

    if (transcript != NULL && tr_cap > 0)
    {
        transcript[0] = '\0';
    }

    esp_err_t err = j1939_pgn_latest(pgn, (sa >= 0) ? sa : J1939_ADDR_ANY,
                                     J1939_ADDR_ANY, &info, payload, cap);

    if (err != ESP_OK)
    {
        if (transcript != NULL && tr_cap > 0 && j1939_status(&st) == ESP_OK)
        {
            snprintf(transcript, tr_cap,
                     "> store: PGN %lX%s\n< not in the store (listener %s, "
                     "%u groups from %u sources)\n", (unsigned long)pgn,
                     (sa >= 0) ? " of a pinned source" : "",
                     j1939_state_name(st.state), (unsigned)st.entries,
                     (unsigned)st.sources);
        }

        return ESP_ERR_NOT_FOUND;
    }

    *len = (info.len < cap) ? info.len : cap;

    if (transcript != NULL && tr_cap > 0)
    {
        snprintf(transcript, tr_cap,
                 "> store: PGN %lX%s\n< from source %u, %u bytes, %lu ms "
                 "old, message %lu of this key%s\n", (unsigned long)pgn,
                 (sa >= 0) ? " of a pinned source" : "", (unsigned)info.sa,
                 (unsigned)info.len, (unsigned long)info.age_ms,
                 (unsigned long)info.count,
                 (info.period_ms != 0) ? "" : " (seen once)");
    }

    return ESP_OK;
}

bool ap_runner_run_j1939(const ap_pid_t *pid, int pid_index,
                         const ap_param_t *params, bool tx_ok)
{
    j1939_msg_t info;

    s_looked++;

    if (pid_index < 0 || pid_index >= AP_MAX_PIDS)
    {
        return false;
    }

    if (!ask(pid, tx_ok))
    {
        return false;
    }

    if (j1939_pgn_latest(pid->pgn, (pid->j1939_sa >= 0) ? pid->j1939_sa
                                                         : J1939_ADDR_ANY,
                         J1939_ADDR_ANY, &info, s_msg,
                         sizeof(s_msg)) != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: PGN %lX not in the store", pid->name,
                 (unsigned long)pid->pgn);
        return false;
    }

    row_mark_t *m = &s_mark[pid_index];

    if (m->seen && m->count == info.count && m->sa == info.sa)
    {
        return true; /* nothing newer than what this row published */
    }

    if (m->seen && m->sa != info.sa && info.age_ms > AP_J1939_STALE_MS)
    {
        /* another SOURCE, and an old message: the store's pick moved (a bus
           that fell silent) rather than anything arriving. Not news; the
           row keeps what it published. A new message of the source the row
           follows is news however old the look finds it (a slow group under
           a slow row); a row that never published takes an old message
           once (a group sent at start-up and never again). */
        m->count = info.count;
        m->sa = info.sa;
        return true;
    }

    m->seen = true;
    m->count = info.count;
    m->sa = info.sa;

    size_t len = (info.len < sizeof(s_msg)) ? info.len : sizeof(s_msg);

    /* stamped with the message's own time, not the look's */
    if (ap_runner_publish(pid, params, s_msg, len,
                          esp_timer_get_time() - (int64_t)info.age_ms * 1000))
    {
        s_published++;
    }

    return true;
}
