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
 * @file obd_chip_cmd.c
 * @brief The request->response engine. It is an ordinary fan-out subscriber
 *        (broadcast contract): it receives EVERYTHING the chip prints and,
 *        per design, whatever arrives inside its transaction window up to the
 *        '>' prompt IS the response, interleaved unsolicited noise included.
 *        Framing/matching logic is pure (obd_chip_parse.c) and host-tested,
 *        including chunk-boundary splits and noise interleave.
 */
#include "obd_chip.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/task.h"

#include "obd_gate.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define CMD_QUEUE_DEPTH 64

/* engine's subscriber queue: PSRAM static (§2) */
static uint8_t s_q_storage[CMD_QUEUE_DEPTH * sizeof(obd_chunk_t)] EXT_RAM_BSS_ATTR;
static StaticQueue_t s_q_buf; /* internal: FreeRTOS object */
static QueueHandle_t s_q;

static obd_resp_acc_t s_acc EXT_RAM_BSS_ATTR; /* one in-flight transaction */

/* obd_chip_protocol_save(): real ATSP writes this boot (stats
   `protocol_saves`); the driver allows ONE attempt per boot, and none when
   the chip holds the protocol already (2026-11-10) */
static uint32_t s_protocol_saves;
static uint32_t s_protocol_kept;   /* saves not needed: the chip held it  */
static bool     s_save_attempted;  /* an ATSP went out this boot          */
static bool     s_stored_known;    /* s_stored_proto is the chip's store  */
static char     s_stored_proto;    /* '0'..'9', 'A'..'C'                  */
static char     s_stored_text[OBD_STORED_LEN]; /* in the chip's words     */

/* The request path's memory protection (2026-11-10, obd_chip_request below).
   A switch the engine refused is OWED: the chip is still on the protocol of
   whoever used it before, so bus requests are held until a switch has gone
   through. Written under the transaction hold; read without it (a flag). */
static volatile bool s_switch_owed;
static bool     s_refusal_logged; /* the W line of this stretch went out */
static uint32_t s_memory_refused; /* switches not sent: ATM0 not acknowledged */
static uint32_t s_requests_held;  /* bus requests not sent while one is owed */

uint32_t obd_cmd_protocol_saves(void)
{
    return s_protocol_saves;
}

void obd_cmd_memory_stats(obd_cmd_memory_t *out)
{
    out->refused = s_memory_refused;
    out->held = s_requests_held;
    out->owed = s_switch_owed;
}

void obd_cmd_memory_reset(void)
{
    s_switch_owed = false;
}

uint32_t obd_cmd_protocol_kept(void)
{
    return s_protocol_kept;
}

void obd_cmd_stored_text(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", s_stored_text);
}

void obd_cmd_stored_note(const char *dpn)
{
    char proto = '\0';
    size_t n = 0;

    s_stored_known = obd_parse_dpn(dpn, &proto, NULL);
    s_stored_proto = proto;

    /* the chip's own words for the stats, the STN / vendor forms too */
    for (const char *p = dpn; p != NULL && *p != '\0' && *p != '\r' &&
                              *p != '\n' && n < sizeof(s_stored_text) - 1; p++)
    {
        unsigned char c = (unsigned char)*p;

        /* printable, and safe inside the JSON string of the stats route */
        s_stored_text[n++] = (c < 0x20 || c >= 0x7F || c == '"' || c == '\\')
                                 ? ' ' : (char)c;
    }

    s_stored_text[n] = '\0';
}

#ifdef OBD_CHIP_HOST_TEST
void obd_cmd_test_new_boot(void)
{
    s_save_attempted = false;
    s_stored_known = false;
    s_stored_proto = '\0';
    s_stored_text[0] = '\0';
}
#endif

esp_err_t obd_cmd_engine_init(void)
{
    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(CMD_QUEUE_DEPTH, sizeof(obd_chunk_t),
                                 s_q_storage, &s_q_buf);

        if (s_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }

        /* NOT subscribed here: the engine subscribes per transaction
           (below). A standing subscription filled the 64-slot queue with
           every app line while no request was in flight: an ELM app
           streaming through a bridge for minutes (autopid yielded) made
           the fan-out drop-count this queue and WARN every 100 chunks
           for nothing (2026-09-08). */
    }

    return ESP_OK;
}

static void cmd_engine_done(void)
{
    obd_chip_unsubscribe(s_q);
    obd_gate_release(obd_chip_gate_owner);
    obd_core_release();
}

/** The transaction body shared by obd_chip_request() (guarded) and
 *  obd_chip_protocol_save() (the one deliberate EEPROM write). */
static esp_err_t request_run(const char *cmd, char *resp, size_t resp_len,
                             TickType_t timeout, bool guard)
{
    if (cmd == NULL || resp == NULL || resp_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_q == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (obd_parse_is_monitor_cmd(cmd))
    {
        /* monitor-class commands never end with a prompt: callers must use
           claim(MONITOR) + send() + subscribe instead (README) */
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* EEPROM guard (obd_chip_guard.h): ATSP/ATM1 go out as their RAM
       twins, ATPP/ATSD/ATCV/STWBR never go out. Runs on a copy of the
       command head (the API takes a const string); the rare command
       longer than the copy is guarded on its head and sent whole. */
    size_t cmd_len = strlen(cmd);
    char guarded[256];
    size_t glen = (cmd_len < sizeof(guarded)) ? cmd_len : sizeof(guarded) - 1;

    memcpy(guarded, cmd, glen);
    guarded[glen] = '\0';

    obd_guard_t gv = guard ? obd_chip_guard_cmd(guarded, glen)
                           : OBD_GUARD_PASS;

    if (gv != OBD_GUARD_PASS)
    {
        obd_core_guard_note(gv, cmd, cmd_len);
    }

    if (gv == OBD_GUARD_BLOCKED)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *head = (gv == OBD_GUARD_REWRITTEN) ? guarded : cmd;

    /* async bring-up: never write mid-negotiation, wait within the
       caller's own budget, then time out without touching the wire
       (boot-window callers like the autopid poller just retry) */
    if (!obd_core_bringup_wait(pdTICKS_TO_MS(timeout)))
    {
        return ESP_ERR_TIMEOUT;
    }

    /* COMMAND claim: serializes concurrent requesters; fails fast when a
       MONITOR session holds the chip (policy "manual", task §5) */
    esp_err_t err = obd_core_claim(1 /* CLAIM_COMMAND */, 2000);

    if (err != ESP_OK)
    {
        return err;
    }

    /* obd_gate: this transaction is one bus conversation, serialize
       against the ESP-side ELM engines (released again on every exit
       path below; the fan-out's '>' detection may beat us to it) */
    (void)obd_gate_acquire(obd_chip_gate_owner, OBD_GATE_WAIT_MS);

    /* start clean: drop stale chunks from before our transaction, then
       join the fan-out for exactly this transaction (subscribe BEFORE
       the write so the response's first chunk cannot be missed) */
    obd_chunk_t chunk;

    while (xQueueReceive(s_q, &chunk, 0) == pdTRUE)
    {
    }

    err = obd_chip_subscribe(s_q, "cmd_engine");

    if (err != ESP_OK)
    {
        obd_gate_release(obd_chip_gate_owner);
        obd_core_release();
        return err;
    }

    obd_parse_reset(&s_acc);

    /* send the (guarded) command, appending the CR if the caller omitted it */
    bool needs_cr = (cmd_len == 0 || cmd[cmd_len - 1] != '\r');

    err = obd_uart_write((const uint8_t *)head, glen);

    if (err == ESP_OK && cmd_len > glen)
    {
        err = obd_uart_write((const uint8_t *)cmd + glen, cmd_len - glen);
    }

    if (err == ESP_OK && needs_cr)
    {
        err = obd_uart_write((const uint8_t *)"\r", 1);
    }

    if (err != ESP_OK)
    {
        cmd_engine_done();
        return err;
    }

    /* collect until the prompt or the caller's deadline */
    TickType_t deadline = xTaskGetTickCount() + timeout;

    while (!s_acc.done)
    {
        TickType_t now = xTaskGetTickCount();

        if (now >= deadline)
        {
            cmd_engine_done();
            ESP_LOGD(TAG, "request '%.8s' timed out (%u bytes so far)", cmd,
                     (unsigned)s_acc.len);
            return ESP_ERR_TIMEOUT;
        }

        if (xQueueReceive(s_q, &chunk, deadline - now) == pdTRUE)
        {
            obd_parse_feed(&s_acc, (const char *)chunk.data, chunk.len);
        }
    }

    obd_parse_extract(&s_acc, cmd, resp, resp_len);
    cmd_engine_done();

    if (s_acc.overflow)
    {
        ESP_LOGW(TAG, "response to '%.8s' exceeded %d bytes (truncated)",
                 cmd, OBD_RESP_MAX);
    }

    return ESP_OK;
}

/*
 * The chip's memory function is ON after every reset (the vendor's pin
 * strap; no non-volatile switch exists): with it on, a protocol switch that
 * a request then answers on is written to the chip's non-volatile protocol
 * store, RAM-only `ATTP` or not (bench 2026-11-01, issue #1023: the Audi
 * MEB profile flipped 6 <-> 7 seven times per poll cycle, the store rated
 * for 10k cycles). Legacy's AutoPID sent `atm0` at every start; v6 never
 * did. So every protocol switch on this path gets its own `ATM0` first,
 * whatever reset happened in between (the bring-up's, a nap's, an app's
 * `ATZ`, the chip's own brown-out). One word, one `OK`, about 1 ms.
 *
 * Fail closed (2026-11-10): the switch goes out only when the chip answered
 * that `ATM0` with `OK` and nothing else. A `?` (bytes of an app's
 * unfinished line sat in the chip's line buffer and merged with the word),
 * any other text, or no prompt inside the caller's budget, and the switch
 * is not sent; the caller gets the error. Before, the switch went out
 * regardless, with the memory function in whatever state the chip had it
 * (the host reproduction: test_request.c).
 */

/** A word or an answer on one log line: what is not printable as a space. */
static void one_line(const char *in, char *out, size_t out_len)
{
    size_t n = 0;

    for (; in != NULL && in[n] != '\0' && n < out_len - 1; n++)
    {
        unsigned char c = (unsigned char)in[n];

        out[n] = (c < 0x20 || c >= 0x7F) ? ' ' : (char)c;
    }

    out[n] = '\0';
}

/** `ATM0`, acknowledged. Anything else: the switch @p cmd is refused, owed
 *  from now on, and every holder is told its prelude is due again. */
static esp_err_t memory_off_first(const char *cmd, TickType_t timeout)
{
    char resp[32] = "";
    esp_err_t err = request_run("ATM0", resp, sizeof(resp), timeout, true);

    if (err == ESP_OK && obd_parse_is_ok(resp))
    {
        obd_core_memory_off_note();

        if (s_refusal_logged)
        {
            s_refusal_logged = false;
            ESP_LOGI(TAG, "ATM0 acknowledged again: protocol switches go out");
        }

        return ESP_OK;
    }

    char said[16];
    char word[16];
    char why[48];

    one_line(resp, said, sizeof(said));
    one_line(cmd, word, sizeof(word));

    if (err == ESP_OK)
    {
        snprintf(why, sizeof(why), "the chip answered '%s'", said);
    }
    else
    {
        snprintf(why, sizeof(why), "no answer, %s", esp_err_to_name(err));
    }

    s_memory_refused++;
    s_switch_owed = true;
    obd_chip_settings_changed();

    if (!s_refusal_logged)
    {
        /* the transition once at W, the repeats at D (standard §10) */
        s_refusal_logged = true;
        ESP_LOGW(TAG, "ATM0 not acknowledged (%s): '%s' not sent, bus "
                      "requests wait for a protocol switch", why, word);
    }
    else
    {
        ESP_LOGD(TAG, "ATM0 not acknowledged (%s): '%s' not sent", why, word);
    }

    return (err == ESP_OK) ? ESP_ERR_INVALID_RESPONSE : err;
}

/** The chip took a word: the last line of its answer is `OK` (an echo ahead
 *  of it is an app's ATE1 after its reset, not a refusal). */
static bool answer_ends_ok(const char *resp)
{
    const char *last = resp;

    for (const char *p = resp; *p != '\0'; p++)
    {
        if ((*p == '\r' || *p == '\n') && p[1] != '\0')
        {
            last = p + 1;
        }
    }

    return obd_parse_is_ok(last);
}

esp_err_t obd_chip_request(const char *cmd, char *resp, size_t resp_len,
                           TickType_t timeout)
{
    if (!obd_parse_sets_protocol(cmd))
    {
        if (s_switch_owed && resp != NULL && resp_len > 0 &&
            obd_parse_is_bus_request(cmd))
        {
            /* a switch was refused and none went through since: this would
               go out on the protocol, and at the bit rate, of whoever used
               the chip before. The generation moves again so a holder that
               did not see the refusal sends its prelude, switch included. */
            resp[0] = '\0';
            s_requests_held++;
            obd_chip_settings_changed();
            ESP_LOGD(TAG, "'%.12s' held: a protocol switch is owed", cmd);
            return ESP_ERR_NOT_FINISHED;
        }

        return request_run(cmd, resp, resp_len, timeout, true);
    }

    if (resp == NULL || resp_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* the bring-up gate first, as a plain request would (the hold below
       must not be taken while the bare-UART probes run) */
    if (!obd_core_bringup_wait(pdTICKS_TO_MS(timeout)))
    {
        return ESP_ERR_TIMEOUT;
    }

    /* the pair inside one hold, so no other requester lands between the
       ATM0 and the switch; a caller inside its own transaction keeps it
       (its end() is not ours to call). No hold, no pair: neither word
       goes out. */
    bool own_txn = !obd_core_txn_owned();
    esp_err_t err = own_txn ? obd_chip_txn_begin(timeout) : ESP_OK;

    if (err != ESP_OK)
    {
        resp[0] = '\0';
        return err;
    }

    err = memory_off_first(cmd, timeout);

    if (err == ESP_OK)
    {
        err = request_run(cmd, resp, resp_len, timeout, true);

        if (err == ESP_OK && answer_ends_ok(resp))
        {
            s_switch_owed = false; /* on a protocol somebody asked for */
        }
    }
    else
    {
        resp[0] = '\0'; /* a refused switch leaves no stale answer behind */
    }

    if (own_txn)
    {
        obd_chip_txn_end();
    }

    return err;
}

esp_err_t obd_chip_protocol_save(char proto)
{
    char p = (char)toupper((unsigned char)proto);

    if (!((p >= '1' && p <= '9') || (p >= 'A' && p <= 'C')))
    {
        return ESP_ERR_INVALID_ARG;     /* never ATSP0 (auto) or junk     */
    }

    /* the ONE unguarded command path besides boot provisioning. The check,
       the mark and the word are one step under the hold, so two callers
       cannot both pass the check (2026-11-10). */
    bool own_txn = !obd_core_txn_owned();
    esp_err_t err = own_txn ? obd_chip_txn_begin(pdMS_TO_TICKS(2000)) : ESP_OK;

    if (err != ESP_OK)
    {
        return err;
    }

    char cmd[8];
    char resp[64] = "";

    snprintf(cmd, sizeof(cmd), "ATSP%c", p);

    if (s_stored_known && s_stored_proto == p)
    {
        /* read back at the bring-up, or acknowledged this boot: the chip
           holds it, nothing to write */
        s_protocol_kept++;
        ESP_LOGI(TAG, "%s not sent: the chip's store holds %c already ('%s')",
                 cmd, p, s_stored_text);
    }
    else if (s_save_attempted)
    {
        err = ESP_ERR_INVALID_STATE; /* one attempt per boot (EEPROM budget) */
    }
    else
    {
        /* marked BEFORE the word goes out: the chip acts on it and then
           answers, so a lost answer is no proof that nothing was written.
           What the store holds is unknown from here until the chip
           acknowledges, or the next bring-up reads it back. */
        s_save_attempted = true;
        s_stored_known = false;
        s_stored_text[0] = '\0';
        err = request_run(cmd, resp, sizeof(resp), pdMS_TO_TICKS(2000), false);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "%s (protocol save) not answered: %s; the next "
                          "start reads what the chip holds", cmd,
                     esp_err_to_name(err));
        }
        else if (!answer_ends_ok(resp))
        {
            ESP_LOGW(TAG, "%s (protocol save) not accepted: '%.16s'", cmd,
                     resp);
            err = ESP_FAIL;
        }
        else
        {
            s_protocol_saves++;
            s_stored_known = true;
            s_stored_proto = p;
            snprintf(s_stored_text, sizeof(s_stored_text), "%c", p);
            ESP_LOGI(TAG, "%s saved to the chip's EEPROM (base protocol "
                          "learned once this boot)", cmd);
        }
    }

    if (own_txn)
    {
        obd_chip_txn_end();
    }

    return err;
}

esp_err_t obd_chip_get_version(char *buf, size_t buf_len, TickType_t timeout)
{
    return obd_chip_request("VTVERS", buf, buf_len, timeout);
}
