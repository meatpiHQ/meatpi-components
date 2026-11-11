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
 * @file autopid_baseline.c
 * @brief The chip baseline, split out of autopid_runner.c (2026-10-20,
 *        700-line rule): the prelude (spaces on, headers off, timeout, the
 *        protocol, the functional header, the receive filter cleared), the
 *        chain sender with its reset rule, and the memory the poll and the
 *        test share (ap_base_mem): what the chip is set up for.
 *
 * Two things are owed back to the baseline:
 *
 * - the request header a standard row of a UDS-dialect car borrows with
 *   its own `ATSH` (2026-10-03): the functional header goes back before
 *   any row that does not set one (ap_base_header_give_back);
 * - everything a chain of the tables moves (2026-10-20, issue #964): a
 *   custom row's `ATSH6F1;ATCEA60;ATCRA660;ATFCSH6F1;ATFCSD60300000;ATFCSM1`
 *   stayed in effect, every standard row that followed went to the BMW
 *   gateway, the gateway answered `7F 01 11` and the values froze (bench
 *   2026-10-20: nine standard rows frozen from the second cycle on, 27
 *   service-01 requests at the gateway's address). Every chain is read for
 *   what it sets beyond the baseline (ap_chain_sets) and before the next
 *   row TYPE the baseline goes back (ap_base_restore: the prelude plus one
 *   undo command per thing the prelude does not cover). Within a type a
 *   row still finds what the rows before it set, as it always did.
 *
 * The same is owed to the firmware's OWN requests (2026-11-10): the identity
 * check, the contact probe, the baseline after an app or a tool had the chip
 * and the detection job sent the prelude alone and dropped the memory of
 * what the tables had moved. A profile row's `ATFCSH17FC007B;ATFCSD300000;
 * ATFCSM1` stayed in effect under them: the chip answered the VIN request
 * with its first frame only (`7E8 10 14 49 02 01 31 57 43`, the flow control
 * went to the row's ECU), the identity knew the car by its ECU set alone,
 * and with only such rows in the tables the detection job failed with "no
 * ECU response" (bench, 3 of 3). Every prelude of the firmware's own carries
 * the undo now (send_prelude_clean), and a job that talks to the chip
 * directly asks for it first (ap_runner_undo_tables).
 *
 * Poller-task context, plus the httpd task for test-a-PID while the poller
 * is paused (the chip job serializes them).
 */
#include "autopid_runner_private.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "obd_chip_guard.h"

#include "autopid_proto.h"
#include "autopid_transport.h"

static const char *TAG = "autopid";

#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

static ap_base_mem_t s_mem;

ap_base_mem_t *ap_base_mem(void)
{
    return &s_mem;
}

esp_err_t ap_base_request_tr(const char *cmd, char *resp, size_t resp_len,
                             TickType_t timeout, ap_tr_t *tr)
{
    resp[0] = '\0';
    ap_tr_add(tr, '>', cmd);

    esp_err_t err = ap_be()->request(cmd, resp, resp_len, timeout);

    /* the two refusals of the chip driver's memory protection (obd_chip.h,
       2026-11-10) are not a silent chip: the word never went out */
    ap_tr_add(tr, '<', (err == ESP_ERR_INVALID_RESPONSE)
                           ? "(not sent: the chip did not acknowledge ATM0)"
                    : (err == ESP_ERR_NOT_FINISHED)
                           ? "(not sent: a protocol switch is owed)"
                    : (err != ESP_OK) ? "(no reply)"
                    : (resp[0] == '\0') ? "(empty)" : resp);
    return err;
}

char ap_base_proto(void)
{
    /* as ap_std_prelude() would: the setting, else the current car's
       record, else the search */
    return ap_veh_effective_protocol(ap_core_std_protocol(),
                                     autopid_vehicle_protocol(),
                                     ap_runner_proto_fallback());
}

/** The prelude went out: nothing is borrowed or moved after it. */
static void prelude_out(void)
{
    s_mem.hdr_borrowed = false;
    s_mem.init_pin = '\0';     /* the chip is on the prelude's protocol */
    s_mem.moved = 0;
    s_mem.rxheader_set = false; /* its ATCRA cleared the filter */

    if (s_mem.bus != 0)
    {
        /* every prelude pins the car's protocol: the chip is off the bus a
           row had taken it to (autopid_bus.h) */
        s_mem.bus = 0;
        s_mem.bus_since_us = esp_timer_get_time();
    }

    s_mem.bus_stale = false;
}

/**
 * A prelude of the firmware's own with one undo command behind it per thing
 * the chains of the tables moved (ap_chain_restore): what goes out whenever
 * the baseline is put back. Not after a reset inside a chain: the chip is
 * at its defaults there (after_reset).
 */
static void send_prelude_clean(const char *prelude, ap_tr_t *tr)
{
    char chain[AP_RESTORE_LEN];

    (void)ap_chain_restore(s_mem.moved, prelude, chain, sizeof(chain));

    if (s_mem.moved != 0)
    {
        ESP_LOGD(TAG, "the tables moved the chip (0x%x): baseline back: %s",
                 s_mem.moved, chain);
    }

    ap_base_send_chain(chain, tr);
    prelude_out();

    /* how the replies behind this prelude read: a pre-CAN protocol prints
       three header bytes and a checksum (autopid_resp_precan.c). A row sets
       the hint again for itself; a job of the firmware's own (the identity
       check, the contact probe) reads behind this one */
    char pin = '\0';

    (void)ap_guard_chain_allowed(NULL, prelude, &pin, NULL);
    ap_resp_set_proto_hint(pin);
}

/**
 * A reset inside a chain of the tables (`ATZ`, `ATD`, `ATWS`) leaves the chip
 * on the protocol stored in its EEPROM, the last vehicle DETECTED, and its
 * first request goes out on that whatever bus the device is plugged into
 * (bench 2026-10-05: the 500 kbit/s car's on the 250 kbit/s truck, bus-off
 * 17 ms after the first error). So the baseline prelude goes out right
 * behind the reset and the rest of the chain builds on it.
 * @return false = the bus guard has no verdict for that protocol right now
 * (another car made current under the row): the chain is cut.
 */
static bool after_reset(ap_tr_t *tr)
{
    char proto = ap_base_proto();

    if (!ap_guard_pin_ok(proto))
    {
        s_mem.baseline_sent = false;
        s_mem.chain_cut = true;
        return false;
    }

    ap_base_send_chain(ap_veh_prelude_for(proto), tr);
    prelude_out();
    return true;
}

/**
 * One element of a chain to the chip. `init` = a chain OF THE TABLES (a
 * type's or a row's init, a tested init): the EEPROM guard and the
 * print-format strip (ATH0 / ATS0 / ATE1 dropped, issue #1014). A chain of
 * the firmware's own (the prelude, the restore) gets the guard alone: the
 * K-line and J1850 preludes carried ATH0 on purpose until 2026-11-12, and the
 * strip blanked it into an empty command the chip answered with `?`
 * (2026-10-26, found behind discussion #1018 on the no-car prelude, then
 * `ATS1;ATH0;ATST96;ATTP0`, sent after the current car was forgotten: the
 * chip got `ATS1`, ``, `ATST96`, `ATTP0`; that prelude is headers-on now,
 * ap_veh_prelude_for()). An element the sanitizing emptied is not sent.
 * @return false = a reset inside the chain met no bus guard verdict: the
 * chain is cut.
 */
static bool send_element(char *cmd, size_t n, ap_tr_t *tr, bool init)
{
    if (init)
    {
        ap_init_sanitize(cmd); /* spare the chip's EEPROM, drop ATH0 ... */
    }
    else
    {
        (void)obd_chip_guard_cmd(cmd, n); /* spare the chip's EEPROM */
    }

    size_t len = strlen(cmd);

    if (len == 0)
    {
        return true;
    }

    char resp[64];

    (void)ap_base_request_tr(cmd, resp, sizeof(resp), AP_INIT_TIMEOUT, tr);

    return !(ap_guard_cmd_resets(cmd, len) && !after_reset(tr));
}

static void send_elements(const char *chain, ap_tr_t *tr, bool init)
{
    char cmd[AP_INIT_LEN];
    size_t n = 0;

    for (const char *p = chain;; p++)
    {
        if (*p == ';' || *p == '\0')
        {
            if (n > 0)
            {
                cmd[n] = '\0';

                if (!send_element(cmd, n, tr, init))
                {
                    return;
                }

                n = 0;
            }

            if (*p == '\0')
            {
                break;
            }
        }
        else if (n < sizeof(cmd) - 1)
        {
            cmd[n++] = *p;
        }
    }
}

void ap_base_send_chain(const char *chain, ap_tr_t *tr)
{
    send_elements(chain, tr, false);
}

void ap_base_send_init(const char *chain, ap_tr_t *tr)
{
    send_elements(chain, tr, true);
    s_mem.moved |= ap_chain_sets(chain);
}

/**
 * Send the baseline prelude, when the bus guard's verdict covers the
 * protocol it pins. The protocol is read ONCE and the prelude built from
 * that reading: the current car can change under this task (an activation
 * from the UI, in the HTTP task), and a prelude for the new car's protocol
 * sent on the strength of the old car's verdict transmits at the wrong bit
 * rate on a live bus (bench 2026-10-05: a 500 kbit/s car activated on a 250
 * kbit/s truck; the truck's adapter was bus-off 40 ms later).
 */
bool ap_base_send_baseline(ap_tr_t *tr)
{
    char proto = ap_base_proto();

    if (!ap_guard_pin_ok(proto))
    {
        ESP_LOGD(TAG, "baseline for protocol %c waits for the bus guard",
                 proto);
        s_mem.baseline_sent = false;
        return false;
    }

    send_prelude_clean(ap_veh_prelude_for(proto), tr);
    return true;
}

void ap_runner_restore_baseline(void)
{
    /* the app's ATZ/ATS0/ATH0/ATSH/ATCRA are all still in effect: put
       back what the parser and the std PIDs assume (the same prelude
       the std scan uses - ATTP not ATSP, so no EEPROM write), then let
       the next poll replay the configured type/PID inits on top. A
       prelude the guard has no verdict for is left to the next poll. */
    (void)ap_base_send_baseline(NULL);
    ap_runner_reset();
}

void ap_runner_send_prelude(const char *prelude)
{
    send_prelude_clean(prelude, NULL);
}

void ap_runner_undo_tables(void)
{
    if (s_mem.moved == 0)
    {
        return;
    }

    char chain[AP_RESTORE_LEN];

    (void)ap_chain_restore(s_mem.moved, "", chain, sizeof(chain));
    ESP_LOGD(TAG, "the tables moved the chip (0x%x): put back for the job: %s",
             s_mem.moved, chain);
    ap_base_send_chain(chain, NULL);
    s_mem.moved = 0;
}

void ap_runner_note_moved(const char *chain)
{
    if (chain != NULL)
    {
        s_mem.moved |= ap_chain_sets(chain);
    }
}

void ap_runner_send_init(const char *init)
{
    ap_base_send_init(init, NULL);
}

uint32_t ap_base_chip_gen(void)
{
    return (ap_be()->settings_generation != NULL)
               ? ap_be()->settings_generation() : 0;
}

bool ap_runner_baseline_ensure(void)
{
    /* boot baseline, once: spaces/headers/timeout + the protocol the
       setting (or the current car's record under "0") names, BEFORE any
       user init so the inits still win. Without it the chip polled on
       whatever its EEPROM held until the first ELM-app pause
       (2026-10-01). Re-armed by ap_runner_rebaseline() on a car switch,
       and by the chip's settings generation (2026-10-28): a fresh
       bring-up or another holder's reset of the chip (the CAN self-test's
       ATD) leaves the chip at its defaults while this memory said the
       prelude was in effect. */
    uint32_t gen = ap_base_chip_gen();

    if (!s_mem.baseline_sent || s_mem.chip_gen != gen)
    {
        if (!ap_base_send_baseline(NULL))
        {
            return false;
        }

        s_mem.baseline_sent = true;
        s_mem.chip_gen = gen;
    }

    return true;
}

void ap_runner_baseline_invalidate(void)
{
    s_mem.baseline_sent = false;
}

bool ap_base_row_borrows_header(int type, const char *init)
{
    return type == AP_PID_STD && ap_dialect_init_sets_header(init);
}

void ap_base_header_give_back(ap_tr_t *tr)
{
    char proto = ap_base_proto();
    const char *func = ap_veh_func_header(proto);

    if (func != NULL)
    {
        char resp[64];

        (void)ap_base_request_tr(func, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                 tr);
        s_mem.moved &= ~AP_SETS_HEADER;
    }
    else if (ap_guard_pin_ok(proto))
    {
        /* the chip's search (it picks its own headers), or a protocol
           without a functional header here: the whole prelude, which pins
           that protocol (hence the guard's word, as for the baseline) */
        send_prelude_clean(ap_veh_prelude_for(proto), tr);
    }

    s_mem.hdr_borrowed = false;
}

bool ap_base_restore(ap_tr_t *tr)
{
    char proto = ap_base_proto();

    if (!ap_guard_pin_ok(proto))
    {
        ESP_LOGD(TAG, "baseline restore for protocol %c waits for the bus "
                      "guard", proto);
        return false;
    }

    send_prelude_clean(ap_veh_prelude_for(proto), tr);
    return true;
}

/* A K-line session and nothing to ask for a while: one short request keeps
   the ECU from ending it (ISO 9141-2 and ISO 14230: 5 s without a request;
   the chip sends nothing by itself, bench 2026-11-12). Without it every row
   of a period over 4.5 s would start the bus again, 2.8 s each time. */
static int64_t s_last_keep_us;

bool ap_runner_kline_keep_due(uint32_t wait_ms)
{
    uint32_t idle = (ap_be()->kline_idle_ms != NULL) ? ap_be()->kline_idle_ms()
                                                     : UINT32_MAX;
    uint32_t since = (s_last_keep_us == 0)
                         ? UINT32_MAX
                         : (uint32_t)((esp_timer_get_time() - s_last_keep_us)
                                      / 1000);

    /* not while the chip is on another bus for a row (autopid_bus.h) */
    return s_mem.bus == 0 && ap_proto_kline_keep(idle, wait_ms, since);
}

void ap_runner_kline_keep(void)
{
    char resp[64];

    /* mode 01 PID 00 is the one request every OBD ECU answers; the digit
       ends the chip's wait at the first answer */
    s_last_keep_us = esp_timer_get_time();
    (void)ap_be()->request("01001", resp, sizeof(resp), pdMS_TO_TICKS(1500));
}

bool ap_base_restore_on(char proto, ap_tr_t *tr)
{
    const ap_proto_t *p = ap_proto(proto);

    if (p == NULL || p->start_ms == 0 || s_mem.init_pin != proto ||
        !ap_guard_pin_ok(proto))
    {
        return false;
    }

    send_prelude_clean(ap_veh_prelude_for(proto), tr);
    s_mem.init_pin = proto; /* a protocol of the tables still, not the car's */
    return true;
}

char ap_base_proto_now(void)
{
    return (s_mem.init_pin != '\0') ? s_mem.init_pin : ap_base_proto();
}

char ap_base_chains_pin(const char *type_init, const char *init,
                        const char *cmd)
{
    const char *const chains[] = { type_init, init, cmd };
    char pin = '\0';

    for (size_t i = 0; i < sizeof(chains) / sizeof(chains[0]); i++)
    {
        char last = '\0';

        if (chains[i] != NULL && chains[i][0] != '\0')
        {
            (void)ap_guard_chain_allowed(NULL, chains[i], &last, NULL);
            pin = (last != '\0') ? last : pin;
        }
    }

    return pin;
}

void ap_base_pin_header(char before, const char *type_init, const char *init,
                        ap_tr_t *tr)
{
    unsigned sets = ap_chain_sets(type_init) | ap_chain_sets(init);
    const char *hdr = ap_veh_pin_header(before,
                                        ap_base_chains_pin(type_init, init,
                                                           NULL),
                                        (sets & AP_SETS_HEADER) != 0);

    if (hdr != NULL)
    {
        char resp[64];

        (void)ap_base_request_tr(hdr, resp, sizeof(resp), AP_INIT_TIMEOUT, tr);
        s_mem.moved |= AP_SETS_HEADER; /* the baseline's is owed back */
    }
}
