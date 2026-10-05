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
 * @file autopid_runner.c
 * @brief The chip-facing runner: one PID poll end to end, init strings
 *        on type/PID transitions, ATCRA rxheader management, request,
 *        payload parse + cross-talk guard, then EVERY enabled parameter
 *        from the ONE response (fix #1) into the cache
 *        (ap_runner_publish(), autopid_publish.c).
 *
 * Owns the "what is the chip currently set up for" memory. Poller-task
 * context only (plus ap_runner_reset from the scan job / settings). First
 * contact (which car is this) lives in autopid_contact.c.
 *
 * The request header is part of that memory since 2026-10-03: the standard
 * rows of a UDS-dialect car (ISO 27145 / SAE J1979-2) address their ECU
 * physically, each row's init carrying its `ATSH`. Such a row BORROWS the
 * header: before any row that does not set one itself the functional
 * header of the protocol is put back, so a custom or specific row finds
 * the same baseline as on an OBD-II car. Only standard rows are tracked: a
 * header set by a custom or specific init stays, as it always did.
 *
 * So does a PROTOCOL a custom or specific init sets (`ATSP6`), with one
 * condition since 2026-10-05: the bus guard must allow it on this bus, when
 * it is sent and before every request that goes out on it (row_pins_ok).
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "obd_chip.h"

#include "autopid_transport.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

#define AP_REQ_TIMEOUT   pdMS_TO_TICKS(1500)
#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

/* chip-state memory: which type/PID the chip is currently set up for */
static char s_type_init[3][AP_INIT_LEN];
static int  s_last_type = -1;
static int  s_last_pid = -1;
static bool s_rxheader_set;

static bool s_baseline_sent;    /* the boot prelude went out             */
static bool s_hdr_borrowed;     /* a standard row's init set the request
                                   header: the functional one is owed back */
static char s_init_pin;         /* the CAN protocol a chain of the TABLES
                                   left the chip on ('\0' = the baseline's):
                                   the bus guard is asked for it again
                                   before every request (row_pins_ok)      */

static const char *const TYPE_INIT_NAME[3] = { "std_init", "custom_init",
                                               "specific_init" };

void ap_runner_set_type_init(int type, const char *init)
{
    if (type >= 0 && type < 3)
    {
        snprintf(s_type_init[type], sizeof(s_type_init[type]), "%s",
                 (init != NULL) ? init : "");
    }
}

void ap_runner_reset(void)
{
    /* someone else (std scan, settings apply) changed protocol/header
       state under the chip: replay inits on the next poll */
    s_last_type = -1;
    s_last_pid = -1;
    s_rxheader_set = false;
}

/** One chip exchange, logged into the transcript when one is wanted. */
static esp_err_t request_tr(const char *cmd, char *resp, size_t resp_len,
                            TickType_t timeout, ap_tr_t *tr)
{
    resp[0] = '\0';
    ap_tr_add(tr, '>', cmd);

    esp_err_t err = ap_be()->request(cmd, resp, resp_len, timeout);

    ap_tr_add(tr, '<', (err != ESP_OK) ? "(no reply)"
                    : (resp[0] == '\0') ? "(empty)" : resp);
    return err;
}

static bool s_chain_cut;        /* a chain was stopped behind a reset (no
                                   verdict for the protocol in effect)     */

static void send_init_tr(const char *init, ap_tr_t *tr);
static char baseline_proto(void);

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
    char proto = baseline_proto();

    if (!ap_guard_pin_ok(proto))
    {
        s_baseline_sent = false;
        s_chain_cut = true;
        return false;
    }

    send_init_tr(ap_veh_prelude_for(proto), tr);
    s_hdr_borrowed = false;
    s_init_pin = '\0';
    return true;
}

/** Send a ';'-separated init string, one command at a time. A chain that
 *  resets the chip gets the baseline prelude behind the reset; cut there
 *  when the guard says no (s_chain_cut). */
static void send_init_tr(const char *init, ap_tr_t *tr)
{
    char cmd[AP_INIT_LEN];
    size_t n = 0;

    for (const char *p = init;; p++)
    {
        if (*p == ';' || *p == '\0')
        {
            if (n > 0)
            {
                cmd[n] = '\0';
                ap_init_sanitize(cmd); /* spare the chip's EEPROM */

                char resp[64];

                (void)request_tr(cmd, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                 tr);

                if (ap_guard_cmd_resets(cmd, n) && !after_reset(tr))
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

static void send_init(const char *init)
{
    send_init_tr(init, NULL);
}

/** The prelude: spaces on, headers off, timeout, protocol, the
 *  functional header, the receive filter cleared. Nothing is borrowed
 *  after it. */
static void send_prelude(const char *prelude)
{
    send_init(prelude);
    s_hdr_borrowed = false;
    s_init_pin = '\0'; /* the chip is on the prelude's protocol again */
}

/**
 * May this row go out? The tables can pin the chip to a CAN protocol
 * themselves (`ATSP6` in a row's init, in a type's init, as a row's cmd:
 * most vehicle profiles do) and the bus guard's verdict is about the
 * protocol autopid pins, not about theirs. So every protocol the chains
 * about to be sent set must be allowed on the bus as the guard last saw it;
 * and when they set none, the one an earlier chain left the chip on must
 * still be (a bus asleep when it was sent may have woken up at another bit
 * rate). Refused = NOTHING of the row is sent: neither its protocol, nor
 * its header on the protocol in effect (an 11-bit header on a truck's
 * 29-bit protocol makes a frame the network has a meaning for), nor the
 * request. Bench 2026-10-05: a row with `ATSP6` tested on a 250 kbit/s
 * truck went out at 500, the truck's adapter was bus-off 19 ms after its
 * first error and stayed there, the chip left on protocol 6 for every poll
 * that followed.
 *
 * @p name NULL = the row under test (test-a-PID). @p type_init / @p init:
 * NULL when not due. @p pin_after: the protocol of the tables the chip sits
 * on once the chains went out ('\0' = the baseline's).
 */
static bool row_pins_ok(const char *name, int type, const char *type_init,
                        const char *init, const char *cmd, bool job,
                        char *pin_after)
{
    char owner[AP_NAME_LEN + 24] = "";
    char pin = '\0';
    char last = '\0';

    if (type_init != NULL && type_init[0] != '\0')
    {
        if (!ap_guard_chain_ok(type_init,
                               (type >= 0 && type < 3) ? TYPE_INIT_NAME[type]
                                                       : "the type's init",
                               job, &last))
        {
            return false;
        }

        pin = (last != '\0') ? last : pin;
    }

    if (init != NULL && init[0] != '\0')
    {
        if (name != NULL)
        {
            snprintf(owner, sizeof(owner), "the init of row %s", name);
        }

        if (!ap_guard_chain_ok(init, (name != NULL) ? owner
                                                    : "the tested row's init",
                               job, &last))
        {
            return false;
        }

        pin = (last != '\0') ? last : pin;
    }

    if (name != NULL)
    {
        snprintf(owner, sizeof(owner), "row %s", name);
    }

    if (!ap_guard_chain_ok(cmd, (name != NULL) ? owner : "the tested row",
                           job, &last))
    {
        return false;
    }

    if (pin == '\0' && last == '\0' && s_init_pin != '\0')
    {
        /* nothing of this row sets a protocol: its request goes out on the
           one an earlier chain left behind */
        const char standing[] = { 'A', 'T', 'T', 'P', s_init_pin, '\0' };

        if (!ap_guard_chain_ok(standing, "an earlier row's init", job, NULL))
        {
            /* the prelude takes the chip off it, the inits are due again
               (and are judged when they are) */
            s_baseline_sent = false;
            ap_runner_reset();
            return false;
        }

        pin = s_init_pin;
    }

    *pin_after = (last != '\0') ? last : pin;
    return true;
}

/** The protocol the baseline prelude pins right now (as ap_std_prelude()
 *  would: the setting, else the current car's record, else the search). */
static char baseline_proto(void)
{
    return ap_veh_effective_protocol(ap_core_std_protocol(),
                                     autopid_vehicle_protocol(),
                                     ap_runner_proto_fallback());
}

/**
 * Send the baseline prelude, when the bus guard's verdict covers the
 * protocol it pins. The protocol is read ONCE and the prelude built from
 * that reading: the current car can change under this task (an activation
 * from the UI, in the HTTP task), and a prelude for the new car's protocol
 * sent on the strength of the old car's verdict transmits at the wrong bit
 * rate on a live bus (bench 2026-10-05: a 500 kbit/s car activated on a 250
 * kbit/s truck; the truck's adapter was bus-off 40 ms later).
 * @return false = not sent, the baseline is due.
 */
static bool baseline_send(void)
{
    char proto = baseline_proto();

    if (!ap_guard_pin_ok(proto))
    {
        ESP_LOGD(TAG, "baseline for protocol %c waits for the bus guard",
                 proto);
        s_baseline_sent = false;
        return false;
    }

    send_prelude(ap_veh_prelude_for(proto));
    return true;
}

void ap_runner_restore_baseline(void)
{
    /* the app's ATZ/ATS0/ATH1/ATSH/ATCRA are all still in effect: put
       back what the parser and the std PIDs assume (the same prelude
       the std scan uses - ATTP not ATSP, so no EEPROM write), then let
       the next poll replay the configured type/PID inits on top. A
       prelude the guard has no verdict for is left to the next poll. */
    (void)baseline_send();
    ap_runner_reset();
}

void ap_runner_send_prelude(const char *prelude)
{
    send_prelude(prelude);
}

bool ap_runner_baseline_ensure(void)
{
    /* boot baseline, once: spaces/headers/timeout + the protocol the
       setting (or the current car's record under "0") names, BEFORE any
       user init so the inits still win. Without it the chip polled on
       whatever its EEPROM held until the first ELM-app pause
       (2026-10-01). Re-armed by ap_runner_rebaseline() on a car switch. */
    if (!s_baseline_sent)
    {
        if (!baseline_send())
        {
            return false;
        }

        s_baseline_sent = true;
    }

    return true;
}

void ap_runner_baseline_invalidate(void)
{
    s_baseline_sent = false;
}

/** A standard row that sets its own request header borrows it. */
static bool row_borrows_header(int type, const char *init)
{
    return type == AP_PID_STD && ap_dialect_init_sets_header(init);
}

/** Put the functional header of the protocol in effect back. */
static void header_give_back(ap_tr_t *tr)
{
    char proto = baseline_proto();
    const char *func = ap_veh_func_header(proto);

    if (func != NULL)
    {
        char resp[64];

        (void)request_tr(func, resp, sizeof(resp), AP_INIT_TIMEOUT, tr);
    }
    else if (ap_guard_pin_ok(proto))
    {
        /* the chip's search (it picks its own headers), or a protocol
           without a functional header here: the whole prelude, which pins
           that protocol (hence the guard's word, as for the baseline) */
        send_init_tr(ap_veh_prelude_for(proto), tr);
        s_init_pin = '\0';
    }

    s_hdr_borrowed = false;
}

esp_err_t ap_runner_test(int type, const char *type_init, const char *init,
                         const char *rxheader, const char *cmd, char *raw,
                         size_t raw_len, int64_t *elapsed_us,
                         char *transcript, size_t transcript_len)
{
    /* ONE-SHOT through the same chip choreography as a real poll
       (test-a-PID, §11): the type init chain (what the poller sends when
       it switches to this PID's type), the per-PID init, ATCRA, the
       request, ATCRA off. Caller must hold the poller paused
       (ap_core_scan_pause) and this leaves the chip state dirty on
       purpose, unpausing runs ap_runner_reset(). The baseline prelude
       goes out first when none went out since the chip was last reset.
       httpd-task context (internal stack). */
    char resp[64];
    ap_tr_t tr = { transcript, transcript_len, 0 };
    bool borrows = row_borrows_header(type, init);
    char cmd_s[AP_CMD_LEN];
    char pin_after = '\0';

    if (transcript != NULL && transcript_len > 0)
    {
        transcript[0] = '\0';
    }

    snprintf(cmd_s, sizeof(cmd_s), "%s", cmd);
    ap_init_sanitize(cmd_s); /* the tested cmd can be an AT command */

    if (!s_baseline_sent)
    {
        /* the first chip traffic of this boot (Automate off, or nothing
           polled yet), or the first after a reset: the chip sits on the
           protocol STORED in it, not on the one in effect, and its request
           went out on that one about every third time (bench 2026-10-05).
           The baseline first, as before a poll (the handler has asked the
           bus guard for it). */
        char proto = baseline_proto();

        if (ap_guard_pin_ok(proto))
        {
            send_init_tr(ap_veh_prelude_for(proto), &tr);
            s_hdr_borrowed = false;
            s_init_pin = '\0';
            s_baseline_sent = true;
        }
    }

    if (s_hdr_borrowed && !borrows)
    {
        header_give_back(&tr);  /* as a poll of this row would */
    }

    if (!row_pins_ok(NULL, type, type_init, init, cmd_s, true, &pin_after))
    {
        ap_tr_add(&tr, '!', "not sent: a protocol this row sets is not the "
                         "vehicle bus's (the bus guard)");
        return ESP_ERR_NOT_ALLOWED;
    }

    if (type_init != NULL && type_init[0] != '\0')
    {
        send_init_tr(type_init, &tr);
    }

    if (init != NULL && init[0] != '\0')
    {
        send_init_tr(init, &tr);
    }

    if (s_chain_cut)
    {
        s_chain_cut = false;
        ap_tr_add(&tr, '!', "stopped behind a reset: the vehicle was changed "
                         "while the row went out");
        return ESP_FAIL;
    }

    if (borrows)
    {
        s_hdr_borrowed = true;
    }

    s_init_pin = pin_after;

    bool cra_set = false;

    if (rxheader != NULL && rxheader[0] != '\0')
    {
        char cra[AP_HDR_LEN + 8];

        snprintf(cra, sizeof(cra), "ATCRA%s", rxheader);
        (void)request_tr(cra, resp, sizeof(resp), AP_INIT_TIMEOUT, &tr);
        cra_set = true;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = request_tr(cmd_s, raw, raw_len, AP_REQ_TIMEOUT, &tr);

    if (ap_guard_cmd_resets(cmd_s, strlen(cmd_s)))
    {
        s_baseline_sent = false; /* the chip is on its stored protocol */
    }

    if (elapsed_us != NULL)
    {
        *elapsed_us = esp_timer_get_time() - t0;
    }

    if (cra_set)
    {
        (void)request_tr("ATCRA", resp, sizeof(resp), AP_INIT_TIMEOUT,
                         &tr);
    }

    return err;
}

const char *ap_runner_type_init(int type)
{
    return (type >= 0 && type < 3) ? s_type_init[type] : "";
}

void ap_runner_send_init(const char *init)
{
    send_init(init);
}

bool ap_runner_run(const ap_pid_t *pid, int pid_index,
                   const ap_param_t *params)
{
    if (!ap_runner_baseline_ensure())
    {
        /* the protocol in effect changed since this pass asked the bus
           guard: nothing is sent, the next pass asks again */
        return false;
    }

    /* a standard row that set its own header gives the functional one
       back before a row that does not set one (the file header) */
    bool borrows = row_borrows_header(pid->type, pid->init);

    if (s_hdr_borrowed && !borrows && pid_index != s_last_pid)
    {
        header_give_back(NULL);
    }

    /* the protocols the tables set themselves, before anything of the row
       is sent */
    bool type_due = (pid->type != s_last_type);
    bool row_due = type_due || pid_index != s_last_pid;
    char pin_after = '\0';

    if (!row_pins_ok(pid->name, pid->type,
                     type_due ? s_type_init[pid->type] : NULL,
                     row_due ? pid->init : NULL, pid->cmd, false, &pin_after))
    {
        ESP_LOGD(TAG, "%s: not sent (the bus guard)", pid->name);
        return false;
    }

    /* type init once per type transition (legacy behavior kept) */
    if (pid->type != s_last_type)
    {
        if (s_type_init[pid->type][0] != '\0')
        {
            send_init(s_type_init[pid->type]);
        }

        s_last_type = pid->type;
        s_last_pid = -1; /* force per-PID setup after a type switch */
    }

    if (pid_index != s_last_pid)
    {
        if (pid->init[0] != '\0')
        {
            send_init(pid->init);
        }

        if (borrows)
        {
            s_hdr_borrowed = true;
        }

        char resp[64];

        if (pid->rxheader[0] != '\0')
        {
            char cra[AP_HDR_LEN + 8];

            snprintf(cra, sizeof(cra), "ATCRA%s", pid->rxheader);
            (void)ap_be()->request(cra, resp, sizeof(resp),
                                   AP_INIT_TIMEOUT);
            s_rxheader_set = true;
        }
        else if (s_rxheader_set)
        {
            (void)ap_be()->request("ATCRA", resp, sizeof(resp),
                                   AP_INIT_TIMEOUT);
            s_rxheader_set = false;
        }

        s_last_pid = pid_index;
    }

    if (s_chain_cut)
    {
        /* a reset in an init found the guard without a verdict: the chip is
           on its stored protocol, nothing goes out (the next pass asks) */
        s_chain_cut = false;
        ap_runner_reset();
        return false;
    }

    s_init_pin = pin_after;

    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* poller-task only */

    esp_err_t err = ap_be()->request(pid->cmd, resp, sizeof(resp),
                                     AP_REQ_TIMEOUT);

    if (ap_guard_cmd_resets(pid->cmd, strlen(pid->cmd)))
    {
        s_baseline_sent = false; /* the chip is on its stored protocol */
    }

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: request failed (%s)", pid->name,
                 esp_err_to_name(err));
        return false;
    }

    uint8_t payload[AP_PAYLOAD_MAX];
    size_t payload_len = 0;

    if (ap_resp_to_payload(resp, payload, sizeof(payload), &payload_len)
            != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: no payload in response", pid->name);
        return false;
    }

    /* cross-talk guard: another chip master's response (WS-OBD bridge,
       CLI) can land in our request window, never cache it (Phase 1b) */
    if (!ap_payload_matches_cmd(pid->cmd, payload, payload_len))
    {
        ESP_LOGD(TAG, "%s: response echo mismatch (cross-talk?)",
                 pid->name);
        return false;
    }

    return ap_runner_publish(pid, params, payload, payload_len,
                             esp_timer_get_time());
}
