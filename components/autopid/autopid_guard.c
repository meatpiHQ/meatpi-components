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
 * @file autopid_guard.c
 * @brief The bus guard's glue: before the OBD chip is pinned to a CAN
 *        protocol, ask can_manager what is on the bus (the native
 *        controller listens, it cannot transmit while it does) and let
 *        autopid_bus_guard.c decide.
 *
 * The poller asks before EVERY transmission until the answer is final. It
 * is final once the bus named its bitrate (frames were read) or an ECU
 * answered on the protocol in effect. A silent bus is not final: a vehicle
 * asleep at boot wakes up later, at its own bitrate (bench 2026-10-03: a
 * verdict taken on the silent bus let the pinned chip put 5078 error frames
 * on a 250k bus in the 2 s after its traffic started). So a listen-only
 * watch stays on the bus meanwhile; it costs nothing while the bus is
 * silent and is let go with the final answer. A one-shot chip job
 * (detection, DTC, test-a-PID) asks when it starts.
 *
 * The listener cannot tell the chip's own unanswered requests (nobody ACKs
 * them: error frames only) from foreign traffic it cannot read, and it rests
 * in "unreadable" for a moment after either. But the chip is idle whenever
 * the guard is asked (the caller is the one who would make it talk), so the
 * guard waits: an echo dies, foreign traffic stays (settle_own_echo).
 *
 * The verdict is about the protocol autopid pins. The chains of the tables
 * (a row's init, a type's init, dtc_init, a tested row) can pin one too and
 * go to the chip as written: whoever sends one asks ap_guard_chain_ok()
 * first, which rules on the bus as it was last seen.
 *
 * Never touches flash (the poller's stack is PSRAM).
 */
#include "autopid_private.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "can_manager.h"

static const char *TAG = "autopid";

#define AP_GUARD_FRESH_US (5LL * 1000 * 1000)  /* a job reuses a probe this
                                                  young                     */
#define AP_GUARD_SETTLE_STEP_MS 100            /* between two looks         */
#define AP_GUARD_SETTLE_LOOKS   8              /* ... at most this many     */

static SemaphoreHandle_t s_lock;       /* poller vs the jobs' tasks         */
static StaticSemaphore_t s_lock_buf;   /* internal: FreeRTOS object         */

static ap_bus_t s_bus;                 /* the last probe's answer           */
static ap_bus_t s_ruled;               /* the bus the verdict was taken on:
                                          what the status view reports      */
static int64_t  s_probe_us;            /* 0 = never probed                  */
static bool     s_final;               /* the verdict stands for this boot  */
static bool     s_final_ok;            /* ... and it lets the chip talk     */
static char     s_ruled_proto;         /* the protocol in effect the standing
                                          verdict was taken for ('0' = the
                                          search, also after a SEARCH verdict;
                                          '\0' = none): another protocol in
                                          effect means another look         */
static char     s_ok_proto;            /* ... and the one a prelude may pin:
                                          s_ruled_proto while the verdict lets
                                          the chip talk, '\0' otherwise
                                          (ap_guard_pin_ok)                 */
static bool     s_watching;            /* we hold can_manager's watch       */
static bool     s_parked;              /* the poller may not, at the moment */
static bool     s_chip_sent;           /* the poller transmitted this boot:
                                          "unreadable" may be the echo of our
                                          own unanswered requests from here
                                          on, and is waited out             */
static ap_guard_verdict_t s_verdict;
static char     s_reason[176];

/* chains of the tables that set a protocol this bus cannot take
   (ap_guard_chain_ok): how many were not sent, the last one's sentence, and
   what was refused (one log line per change, not per poll) */
static uint32_t s_refused;
static int64_t  s_refused_us;          /* ... and when the last one was    */
static char     s_refused_proto;
static uint16_t s_refused_kbps;
static char     s_refused_reason[176] EXT_RAM_BSS_ATTR;
static char     s_job_reason[176] EXT_RAM_BSS_ATTR; /* why the last job was
                                                       refused: the verdict's
                                                       sentence or a chain's */

void ap_guard_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

/** The protocol the chip prelude would pin right now, and whose word it is. */
static void effective_protocol(char *proto, bool *pinned)
{
    const char *setting = ap_core_std_protocol();

    *pinned = (setting[0] >= '6' && setting[0] <= '9');
    *proto = ap_veh_effective_protocol(setting, autopid_vehicle_protocol(),
                                       ap_runner_proto_fallback());
}

/** Listen to the bus now (up to about a second). Under s_lock. */
static void probe_now(void)
{
    can_manager_probe_t p;
    ap_bus_t bus = { .kind = AP_BUS_UNKNOWN, .kbps = 0 };

    if (can_manager_probe(&p) == ESP_OK)
    {
        switch (p.result)
        {
        case CAN_PROBE_SILENT:
            bus.kind = AP_BUS_SILENT;
            break;

        case CAN_PROBE_LIVE:
            bus.kind = AP_BUS_LIVE;
            bus.kbps = (uint16_t)p.baud_kbps;
            break;

        case CAN_PROBE_UNREADABLE:
            bus.kind = AP_BUS_UNREADABLE;
            break;

        default:
            break;
        }
    }

    s_bus = bus;
    s_probe_us = esp_timer_get_time();
}

/**
 * "Unreadable" after the chip has talked may be the echo of its own
 * unanswered request (bench 2026-10-03: the guard parked the chip on its own
 * NO DATA polls, saw silence, let it go, and looped). The chip is idle now,
 * so the answer is to wait and look again: the listener says "silent" only
 * after 400 ms in which it heard nothing at all, which an echo allows and
 * foreign traffic does not. What is still unreadable when the looks are used
 * up is foreign, and the verdict says so. Under s_lock; about half a second
 * for an echo, up to two for a bus that stays unreadable.
 */
static void settle_own_echo(void)
{
    for (int i = 0; i < AP_GUARD_SETTLE_LOOKS; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(AP_GUARD_SETTLE_STEP_MS));
        probe_now();

        if (s_bus.kind != AP_BUS_UNREADABLE)
        {
            return;
        }
    }
}

/** Decide for the protocol in effect; a SEARCH verdict is carried out here
 *  (the runner's prelude turns to ATTP0). Under s_lock. */
static ap_guard_verdict_t decide_now(void)
{
    char proto;
    bool pinned;
    char reason[sizeof(s_reason)];

    effective_protocol(&proto, &pinned);

    if (s_bus.kind == AP_BUS_UNREADABLE && s_chip_sent)
    {
        settle_own_echo();
    }

    ap_guard_verdict_t v = ap_guard_decide(&s_bus, proto, pinned);

    (void)ap_guard_reason(&s_bus, proto, pinned, reason, sizeof(reason));
    s_ruled = s_bus;

    /* On a silent bus the protocol asked for is sent as before (a
       gatewayed OBD port answers only when asked) and the verdict stays
       provisional: the watch catches a bus that wakes up at another
       bitrate before the next transmission. Not the chip's search: on a
       quiet bus it needs about 7 s when its base protocol differs (bench
       2026-10-03), far beyond a poll's timeout. */

    /* one line per change of mind, not per look */
    if (v != s_verdict || strcmp(reason, s_reason) != 0 || s_reason[0] == '\0')
    {
        if (v == AP_GUARD_ALLOW)
        {
            ESP_LOGI(TAG, "bus guard: %s", reason);
        }
        else
        {
            ESP_LOGW(TAG, "bus guard: %s", reason);
        }
    }

    snprintf(s_reason, sizeof(s_reason), "%s", reason);
    s_verdict = v;

    if (v == AP_GUARD_SEARCH)
    {
        ap_runner_force_search();
    }

    return v;
}

/** Let the watch go (under s_lock). */
static void watch_off(void)
{
    if (s_watching)
    {
        s_watching = false;
        (void)can_manager_watch(false);
    }
}

/** The verdict just taken is about the protocol in effect NOW (decide_now()
 *  may have turned it into the search). Under s_lock. */
static void ruled_for_now(bool ok)
{
    char proto;
    bool pinned;

    effective_protocol(&proto, &pinned);
    s_ruled_proto = proto;
    s_ok_proto = ok ? proto : '\0';
}

bool ap_guard_poll_ok(void)
{
    if (s_lock == NULL)
    {
        return true; /* not initialised: behave as before the guard */
    }

    char proto;
    bool pinned;

    effective_protocol(&proto, &pinned);

    /* a final verdict stands for the protocol it was taken for. Another car
       made current (its record names another protocol), a fallback given up:
       the bus is looked at again, for that one. Until 2026-10-05 the first
       verdict stood for the boot and covered whatever was pinned next. */
    if (s_final && proto == s_ruled_proto)
    {
        /* this pass pins that protocol or nothing (a contact probe's
           candidate or a job may have borrowed the permission meanwhile) */
        s_ok_proto = s_final_ok ? proto : '\0';
        return s_final_ok;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_final = false;

    if (!s_watching)
    {
        /* from here the bus is listened to all the time: the looks below
           answer at once (the first one waits for the 400 ms that make a
           bus "silent") */
        (void)can_manager_watch(true);
        s_watching = true;
    }

    probe_now();

    bool ok = (decide_now() != AP_GUARD_PARK);

    if (ok)
    {
        s_chip_sent = true; /* the caller transmits next */
    }

    ruled_for_now(ok);

    if (s_bus.kind == AP_BUS_LIVE)
    {
        /* the bus named its bitrate: that does not change under a device
           that stays plugged in, so the verdict stands for this boot (for
           the protocol it was taken for) */
        s_final = true;
        s_final_ok = ok;
        watch_off();
    }

    s_parked = !ok;
    xSemaphoreGive(s_lock);
    return ok;
}

void ap_guard_proven(void)
{
    if (s_final || s_lock == NULL)
    {
        return;
    }

    /* an ECU answered on the protocol the poll was sent on, which is the
       one this pass's verdict allowed (ap_guard_pin_ok saw to that): its
       bitrate is the bus's. Not "the protocol in effect now": another car
       may have been made current while the request was out. */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_ok_proto != '\0')
    {
        s_final = true;
        s_final_ok = true;
        s_ruled_proto = s_ok_proto;
        s_parked = false;
        watch_off();
    }

    xSemaphoreGive(s_lock);
}

bool ap_guard_pin_ok(char proto)
{
    if (s_lock == NULL)
    {
        return true;
    }

    return ap_guard_pin_allowed(proto, s_ok_proto);
}

bool ap_guard_job_ok(char *reason, size_t cap)
{
    if (s_lock == NULL)
    {
        return true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* the final answer needs no new look; otherwise a look this young is
       reused (the periodic DTC check asks on every tick), and with the
       poller's watch on the bus a new one answers at once */
    if (!s_final && (s_probe_us == 0 ||
                     esp_timer_get_time() - s_probe_us > AP_GUARD_FRESH_US))
    {
        probe_now();
    }

    ap_guard_verdict_t v = decide_now();

    snprintf(s_job_reason, sizeof(s_job_reason), "%s", s_reason);

    /* the job's own prelude (its baseline restore) may pin what this
       verdict was taken for; the poller's standing verdict is not touched
       (its next pass sets the permission back to its own) */
    char proto;
    bool pinned;

    effective_protocol(&proto, &pinned);
    s_ok_proto = (v != AP_GUARD_PARK) ? proto : '\0';

    if (reason != NULL && cap > 0)
    {
        snprintf(reason, cap, "%s", s_reason);
    }

    xSemaphoreGive(s_lock);
    return v != AP_GUARD_PARK;
}

void ap_guard_bus(ap_bus_t *out)
{
    if (out == NULL)
    {
        return;
    }

    if (s_lock == NULL)
    {
        out->kind = AP_BUS_UNKNOWN;
        out->kbps = 0;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_bus;
    xSemaphoreGive(s_lock);
}

bool ap_guard_chain_ok(const char *chain, const char *owner, bool job,
                       char *last)
{
    ap_bus_t bus;
    char refused = '\0';

    if (s_lock == NULL)
    {
        /* not initialised: behave as before the guard */
        (void)ap_guard_chain_allowed(NULL, chain, last, NULL);
        return true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bus = s_bus;
    xSemaphoreGive(s_lock);

    if (ap_guard_chain_allowed(&bus, chain, last, &refused))
    {
        return true;
    }

    char reason[sizeof(s_refused_reason)];

    (void)ap_guard_chain_reason(&bus, refused, owner, reason, sizeof(reason));

    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool news = (s_refused == 0 || refused != s_refused_proto ||
                 bus.kbps != s_refused_kbps);

    s_refused++;
    s_refused_us = esp_timer_get_time();
    s_refused_proto = refused;
    s_refused_kbps = bus.kbps;
    snprintf(s_refused_reason, sizeof(s_refused_reason), "%s", reason);

    if (job)
    {
        snprintf(s_job_reason, sizeof(s_job_reason), "%s", reason);
    }

    xSemaphoreGive(s_lock);

    if (news || job)
    {
        ESP_LOGW(TAG, "bus guard: %s", reason);
    }

    return false;
}

bool ap_guard_candidate_ok(char proto)
{
    if (s_lock == NULL)
    {
        return true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_final)
    {
        /* a provisional picture (a silent bus): look again, and tell the
           echo of our own unanswered probe from foreign traffic */
        probe_now();

        if (s_bus.kind == AP_BUS_UNREADABLE && s_chip_sent)
        {
            settle_own_echo();
        }
    }

    /* the caller pins this protocol itself: there is no search to fall
       back to, so anything but "allow" means "do not send" */
    bool ok = ap_guard_decide(&s_bus, proto, true) == AP_GUARD_ALLOW;

    if (ok)
    {
        s_chip_sent = true;

        /* the caller pins this candidate next; when a car answers on it,
           the store takes it and the baseline is restored on it */
        s_ok_proto = proto;
    }

    xSemaphoreGive(s_lock);
    return ok;
}

void ap_guard_rearm(void)
{
    /* the protocol in effect changed (another car, a new detection): the
       poller asks again before its next prelude, and no prelude pins
       anything until it has. Plain flag writes. */
    s_final = false;
    s_parked = false;
    s_ok_proto = '\0';
    s_ruled_proto = '\0';
}

bool ap_guard_parked(void)
{
    return s_parked;
}

void ap_guard_last_reason(char *out, size_t cap)
{
    if (out != NULL && cap > 0)
    {
        /* a job asks ap_guard_job_ok() first, then for its chains */
        snprintf(out, cap, "%s", (s_job_reason[0] != '\0') ? s_job_reason
                                                            : s_reason);
    }
}

void ap_guard_status_json(cJSON *obj)
{
    if (obj == NULL)
    {
        return;
    }

    cJSON *g = cJSON_AddObjectToObject(obj, "bus_guard");

    if (g == NULL)
    {
        return;
    }

    cJSON_AddStringToObject(g, "bus", ap_bus_kind_name(s_ruled.kind));
    cJSON_AddNumberToObject(g, "bus_kbps", s_ruled.kbps);
    cJSON_AddStringToObject(g, "verdict", ap_guard_verdict_name(s_verdict));
    cJSON_AddBoolToObject(g, "parked", s_parked);
    cJSON_AddStringToObject(g, "reason", s_reason);
    cJSON_AddNumberToObject(g, "refused", s_refused);

    if (s_refused > 0)
    {
        cJSON_AddStringToObject(g, "refused_reason", s_refused_reason);
        cJSON_AddNumberToObject(g, "refused_ms",
                                (double)((esp_timer_get_time() - s_refused_us) /
                                         1000));
    }
}
