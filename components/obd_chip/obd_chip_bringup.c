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
 * @file obd_chip_bringup.c
 * @brief The chip's boot sequence on the bare UART (before the RX task
 *        exists): wake, reset, baud negotiation, the legacy provisioning
 *        (power-on baud, the power-pin switch; the sleep control's is in
 *        obd_chip_provision.c since 2026-11-12). Split out of obd_chip.c
 *        2026-10-03 (700-line rule); obd_chip.c keeps the bring-up task
 *        and its gate.
 */
#include "obd_chip.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "obd_chip_private.h"
#include "obd_chip_reset_line.h" /* this file alone: the last resort below */

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define OBD_INIT_RETRIES 3

/* What the last bare probe saw, prompt or not (issue #1023, 2026-10-28: a
 * chip that is alive but not in ELM mode was reported as silent; a few
 * bytes tell a bootloader or a reboot loop from a dead wire; a chip at
 * 2 Mbaud with its echo ON answers the garbled 115200 bytes with hundreds
 * of bytes of echo read at the wrong speed and no prompt; with the echo
 * off, as the bring-up leaves it, it answers nothing). */
static size_t  s_probe_rx;
static uint8_t s_probe_head[12];
static size_t  s_probe_head_len;

/* the verdict per baud the negotiation tried and got no prompt from, kept
 * for GET /api/obd_chip ("bringup":{"wire":[...]}): the page's banner says
 * what the chip did instead of "not responding" (slot 0 = 2 Mbaud, 1 =
 * 115200; baud 0 = not tried or answered at once) */
static obd_wire_note_t s_wire[2];

/* what this bring-up met at the wake and in the chip's identity (issue #1023) */
static obd_bringup_note_t s_note;

/* the length of the last bare probe's answer: it can hold a NUL */
static size_t s_probe_len;

void obd_bringup_note(obd_bringup_note_t *out)
{
    *out = s_note;
}

bool obd_bringup_store_damaged(void)
{
    return s_note.store_damaged;
}

size_t obd_chip_bringup_wire(obd_wire_note_t *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < 2 && n < max; i++)
    {
        if (s_wire[i].baud != 0)
        {
            out[n++] = s_wire[i];
        }
    }

    return n;
}

/** One request->response on the bare UART (no RX task reads the wire: the
 *  bring-up, or a holder of the exclusive claim). Not reentrant. */
bool obd_bare_probe(const char *cmd, char *resp, size_t resp_len,
                    uint32_t timeout_ms)
{
    /* static: the accumulator's 4 KB response buffer was over half the
       8 KB main-task stack at boot (2026-07-22 stack audit). Safe as a
       static: obd_bare_probe is boot-provisioning only, before the RX
       task exists, single-threaded by construction. */
    static obd_resp_acc_t acc EXT_RAM_BSS_ATTR;
    uint8_t buf[OBD_CHIP_CHUNK_SIZE];

    obd_uart_flush_input();
    obd_uart_write((const uint8_t *)cmd, strlen(cmd));
    obd_parse_reset(&acc);
    s_probe_rx = 0;
    s_probe_head_len = 0;
    s_probe_len = 0;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (!acc.done && xTaskGetTickCount() < deadline)
    {
        int got = obd_uart_read(buf, sizeof(buf), 50);

        if (got > 0)
        {
            for (int i = 0; i < got && s_probe_head_len < sizeof(s_probe_head);
                 i++)
            {
                s_probe_head[s_probe_head_len++] = buf[i];
            }

            s_probe_rx += (size_t)got;
            obd_parse_feed(&acc, (const char *)buf, (size_t)got);
        }
    }

    if (acc.done)
    {
        s_probe_len = obd_parse_extract(&acc, cmd, resp, resp_len);
        return true;
    }

    return false;
}

/** One W line about a probe that got no prompt: silent, or N bytes with
 *  the first of them in hex. The field log then tells a dead wire from a
 *  chip that answers at another baud or in another mode (issue #1023). */
static void wire_note(const char *what, int baud)
{
    obd_wire_note_t *slot = &s_wire[(baud == OBD_CHIP_BAUD) ? 0 : 1];

    slot->baud = baud;
    slot->bytes = (uint32_t)s_probe_rx;
    slot->prompt = false;
    slot->hex[0] = '\0';

    if (s_probe_rx == 0)
    {
        ESP_LOGW(TAG, "%s at %d baud: nothing received", what, baud);
        return;
    }

    char hex[3 * sizeof(s_probe_head) + 1];
    size_t n = 0;

    for (size_t i = 0; i < s_probe_head_len && n < sizeof(hex) - 3; i++)
    {
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%s%02X",
                              (i > 0) ? " " : "", s_probe_head[i]);
    }

    strlcpy(slot->hex, hex, sizeof(slot->hex));
    ESP_LOGW(TAG, "%s at %d baud: %u byte(s) but no prompt (%s%s)", what,
             baud, (unsigned)s_probe_rx, hex,
             (s_probe_rx > s_probe_head_len) ? " ..." : "");
}

/**
 * Reset an awake chip by command, and make sure it happened.
 *
 * Until 2026-10-03 this was one `ATZ` whose prompt was taken as proof. On a
 * software restart of the ESP the chip usually answered that first command
 * with a prompt and no reset (what the last run left in its line buffer, or
 * a glitch of our TX pin, sat in front of it): bring-up took 852 ms instead
 * of 2096 ms on 15 of 30 logged boots, and the chip kept everything the
 * previous run had set, an open CAN session included. A chip holding a
 * session at 500 kbit/s is a node at 500 kbit/s: on a bus at another bitrate
 * it destroys the traffic (bench: 450 error frames a second on a 250k bus,
 * from a device whose firmware believed the chip was fresh).
 *
 * So: one throw-away command first (whatever is in front of it ends with
 * it, as a `?`), then the reset word, and the answer must carry the banner.
 * Twice. Until 2026-11-12 the caller pulsed the reset pin when that failed;
 * a chip that reads ready gets no pulse any more (issue #1023): the caller
 * looks for it at the other UART speed and tries again there, and the pin
 * is left for a chip that answers at neither.
 *
 * The word is `ATWS` since 2026-11-02 (what 4.x sent in place of every
 * `ATZ`): every RAM setting gone, the baud kept, without the LED test,
 * about 40 ms instead of 1.3 s, and without the junk bytes the MIC3624 puts
 * ahead of its `ATZ` banner (5 of 16 tries on the bench, a NUL among them:
 * the "soft reset not confirmed" of every third software restart since
 * 2026-10-19, and a reset pulse plus 1.5 s on top of a chip that had reset
 * fine). The extract behind obd_bare_probe skips such bytes now as well.
 *
 * It is NOT the complete reset it was taken for: on the MIC3624 the warm
 * start leaves an established CAN session standing (2026-11-09: `ATDP`
 * still names the protocol, and the chip stays an active node at that
 * bitrate). So the 2026-10-03 fault was back from 2026-11-02 on: polls at
 * 500 kbit/s, software restarts only, the bus at 250 kbit/s, and a chip the
 * firmware had not written one byte to destroyed 4527 frames in 8 s. The
 * protocol is therefore CLOSED behind the banner (`ATPC`, acknowledged):
 * the session ends, the protocol number and the stored protocol stay as
 * they are (`ATDPN` reads the same before and after), and the next request
 * opens it again in the usual 350 ms. Not acknowledged = not confirmed:
 * the second try; the caller says so when that fails too.
 */
static bool chip_soft_reset(char *resp, size_t resp_len)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        (void)obd_bare_probe("ATI\r", resp, resp_len, 500);

        const size_t heard = s_probe_rx;

        if (!obd_bare_probe("ATWS\r", resp, resp_len, 2500) ||
            strstr(resp, "ELM") == NULL)
        {
            if (heard + s_probe_rx == 0)
            {
                /* not one byte to two words: the chip is not at this
                   speed, and a second try would say the same 3 s later */
                return false;
            }

            continue;
        }

        resp[0] = '\0';

        if (obd_bare_probe("ATPC\r", resp, resp_len, 1000) &&
            strstr(resp, "OK") != NULL)
        {
            return true;
        }

        ESP_LOGW(TAG, "ATPC not acknowledged behind the warm start ('%.12s'): "
                      "the chip may still hold its CAN session", resp);
    }

    return false;
}

/** Look for the chip on the wire: the fixed baud first, then its power-on
 *  default, @p tries at each. True and @p active_baud when it answered; the
 *  bring-up's wire notes for each speed it did not answer at. */
static bool chip_find(int tries, int *active_baud, char *resp, size_t resp_len)
{
    const int try_bauds[2] = { OBD_CHIP_BAUD, OBD_CHIP_FALLBACK_BAUD };

    for (size_t b = 0; b < 2; b++)
    {
        obd_uart_set_baud(try_bauds[b]);

        for (int attempt = 0; attempt < tries; attempt++)
        {
            /* lone CR clears any stale/monitor state (stop byte is space,
               but on a fresh boot CR-flush is what the legacy flow does) */
            obd_uart_write((const uint8_t *)"\r", 1);
            vTaskDelay(pdMS_TO_TICKS(100));

            if (obd_bare_probe("ATI\r", resp, resp_len, 1500))
            {
                *active_baud = try_bauds[b];
                return true;
            }
        }

        wire_note("ATI", try_bauds[b]); /* the last try at this baud */
    }

    return false;
}

void obd_bringup_reset_if_silent(void)
{
    /* Behind a chip firmware update. The chip restarts itself behind
       `VTDLED`; until 2026-11-12 a pulse on the reset line followed 2 s
       later whatever the chip did (legacy). A chip that answers gets none
       any more (issue #1023): the bring-up behind the update warm-starts
       it. Only one that answers at neither speed is left to the line, as
       the last resort. */
    const int first = obd_uart_get_baud();
    const int other = (first == OBD_CHIP_BAUD) ? OBD_CHIP_FALLBACK_BAUD
                                               : OBD_CHIP_BAUD;
    char resp[32];
    bool answers = obd_bare_probe("ATI\r", resp, sizeof(resp), 1500);

    if (!answers)
    {
        obd_uart_set_baud(other);
        answers = obd_bare_probe("ATI\r", resp, sizeof(resp), 1500);
        obd_uart_set_baud(first);
    }

    if (!answers)
    {
        ESP_LOGW(TAG, "chip silent at %d and %d baud behind the update; "
                 "hardware reset, the last resort", first, other);
        (void)obd_pin_reset_pulse(!answers);
    }
}

/** The full wake/reset/negotiate/provision sequence: the pre-2026-07-26
 *  synchronous obd_chip_start() body, run by the bring-up task in
 *  obd_chip.c. @p active_baud: the baud the chip answers at (written as the
 *  sequence learns it). ESP_OK = the chip is up and the RX task runs. */
esp_err_t obd_bringup_run(int *active_baud)
{
    char resp[64];

    memset(s_wire, 0, sizeof(s_wire)); /* this round's verdicts only */
    memset(&s_note, 0, sizeof(s_note));
    obd_pin_wake();

    /* NO RESET INSIDE THE CHIP'S OWN BOOT (issue #1023, 2026-11-12). Behind
       the sleep pin a sleeping chip shows READY at about 113 ms, restarts
       itself at about 235 ms and is ready for good at about 500 ms, or at
       about 680 ms when it has its store to tidy, which it does in the
       first boot behind every 264 store writes. Until this change the
       bring-up waited a fixed 300 ms, read "not ready" inside that boot and
       pulsed the reset line, 63 to 69 ms into the boot: where the tidying
       starts. A reset inside the tidying destroys the chip's store (two
       bench chips; both field signatures). Now it waits until READY has
       been ready without a break for OBD_READY_STEADY_MS, longer than the
       120 ms a waking chip reads ready before it restarts itself, and then
       takes the warm start, as after a power-on. A chip that was awake all
       along (a power-on, a settings save) settles in those 300 ms, as
       before. */
    uint32_t waited = 0;
    obd_settle_verdict_t settled = obd_pin_settle(OBD_WAKE_SETTLE_MS, &waited);

    s_note.wake_ms = waited;
    s_note.wake_ready = (settled == OBD_SETTLE_READY);

    /* THE RESET LINE IS THE LAST RESORT (2026-11-12, Ali: "the pin reset
       should not be allowed unless as a last resort"). Legacy pulsed it
       whenever READY read asleep and sent a soft `ATZ` to an awake chip; v6
       also pulsed an awake chip whose warm start it could not confirm. Now
       nothing here pulses on what READY says. A chip that reads ready is
       reset by command. Whatever READY says, the chip is then asked on the
       wire at both speeds, and only one that answers at neither gets the
       pulse, once, with READY at rest: a chip asleep that the sleep pin
       cannot wake (its PowerEn function off; bench 2026-11-07: nothing else
       wakes it), one an app left at a third UART speed until its next
       reset, one that hangs. A chip that answers although READY reads
       asleep, or behind a READY line that never held still, is reset by
       command further down. reset_done says the chip went through a
       reset. */
    const bool down = (settled == OBD_SETTLE_DOWN);
    bool reset_done = false;

    obd_uart_set_baud(OBD_CHIP_BAUD);

    if (settled == OBD_SETTLE_READY)
    {
        if (waited > OBD_READY_STEADY_MS + 100)
        {
            ESP_LOGI(TAG, "chip woke: READY steady %lu ms behind the sleep pin",
                     (unsigned long)waited);
        }

        ESP_LOGI(TAG, "chip awake; warm start (ATWS) instead of hardware reset");
        reset_done = chip_soft_reset(resp, sizeof(resp));

        if (!reset_done)
        {
            ESP_LOGW(TAG, "soft reset not confirmed at %d baud; looking for "
                     "the chip (no reset pulse: it reads ready)", OBD_CHIP_BAUD);
        }
    }
    else
    {
        /* asleep or stuck, or a READY line that never held still (a chip
           that restarts over and over, one in the middle of a boot): asked
           on the wire first, like any other. One word ahead, so that the
           lone CR of the look repeats it and not what the run before sent
           last. */
        ESP_LOGW(TAG, "chip not ready (READY %s, %lu ms behind the sleep pin); "
                 "asking it on the wire before any reset",
                 down ? "stayed high" : "kept changing", (unsigned long)waited);
        (void)obd_bare_probe("ATI\r", resp, sizeof(resp), 500);
    }

    /* baud negotiation: the fixed baud first, then the chip's power-on
       default. A chip whose READY already says asleep gets one try at each
       speed ahead of the last resort and the full walk behind it; a chip
       that reads ready the other way round. */
    bool alive = chip_find(down ? 1 : OBD_INIT_RETRIES, active_baud, resp,
                           sizeof(resp));

    if (!alive)
    {
        /* the last resort, and the only reset pulse a bring-up can send.
           Noted for the fault code obd_chip_silent, however it ends. */
        s_note.silent = true;

        if (down)
        {
            ESP_LOGW(TAG, "chip not ready; hardware reset, the last resort "
                     "(READY stayed high, no answer at %d or %d baud)",
                     OBD_CHIP_BAUD, OBD_CHIP_FALLBACK_BAUD);
        }
        else
        {
            ESP_LOGW(TAG, "chip silent at %d and %d baud; hardware reset, the "
                     "last resort", OBD_CHIP_BAUD, OBD_CHIP_FALLBACK_BAUD);
        }

        if (obd_pin_reset_pulse(!alive))
        {
            s_note.wake_pulsed = true;
            reset_done = true;
            vTaskDelay(pdMS_TO_TICKS(1500)); /* chip boot time after reset */
            alive = chip_find(down ? OBD_INIT_RETRIES : 1, active_baud, resp,
                              sizeof(resp));
        }
    }

    if (!alive)
    {
        ESP_LOGE(TAG, "chip not responding at %d or %d baud", OBD_CHIP_BAUD,
                 OBD_CHIP_FALLBACK_BAUD);
        return ESP_ERR_NOT_FOUND;
    }

    /* switch the chip to the fixed baud if it woke at the fallback */
    if (*active_baud != OBD_CHIP_BAUD)
    {
        char cmd[24];

        snprintf(cmd, sizeof(cmd), "STSBR %d\r", OBD_CHIP_BAUD);
        obd_uart_write((const uint8_t *)cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(100));
        obd_uart_set_baud(OBD_CHIP_BAUD);

        if (obd_bare_probe("ATI\r", resp, sizeof(resp), 1500))
        {
            *active_baud = OBD_CHIP_BAUD;
            /* legacy: persist the negotiated baud as the chip's
               power-on default (next boot answers at OBD_CHIP_BAUD) */
            obd_bare_probe("STWBR\r", resp, sizeof(resp), 1500);
        }
        else
        {
            obd_uart_set_baud(OBD_CHIP_FALLBACK_BAUD); /* stay where the chip is */
            *active_baud = OBD_CHIP_FALLBACK_BAUD;
            ESP_LOGW(TAG, "STSBR to %d failed; staying at %d", OBD_CHIP_BAUD,
                     OBD_CHIP_FALLBACK_BAUD);
        }
    }

    if (!reset_done)
    {
        /* found, and not reset yet: it sat at the other speed, or it
           answers although READY read asleep or never held still. By
           command again, where the reset pin used to do it; a session the
           run before left open on the vehicle's bus must not outlive this
           (see chip_soft_reset). */
        reset_done = chip_soft_reset(resp, sizeof(resp));

        if (!reset_done)
        {
            ESP_LOGE(TAG, "the chip answers but confirms no reset by command: "
                     "used as it is, a session of the run before may still "
                     "be open (no reset pulse for a chip that answers)");
        }
    }

    /* legacy elm327_powerpin_commands: fw >= V2.3.22 must run with the
       power-pin switch config at 10 (low sleeps, high wakes; v4.52p:
       V2.3.22 or V2.3.24; until 2026-10-28 only a .22 chip got the check).
       The version is kept for GET /api/obd_chip on the way. A wrong value
       is the one EEPROM write of this path: the write acknowledged, left
       OBD_PPSW_SETTLE_MS to settle (legacy's 100 + 500 ms) and read back
       BEFORE the reset (2026-11-07; until then a reset pulse came 0 ms
       after the OK). The reset is an `ATZ` since 2026-11-12: legacy pulsed
       the reset pin here, and a chip that answers gets no pulse any more
       (issue #1023). Should the chip take the new value only at a boot of
       its own, it does at its next wake. An unreadable value is left
       alone: no blind write. */
    bool have_ver = obd_bare_probe("VTVERS\r", resp, sizeof(resp), 1500);
    obd_vtvers_t ver = have_ver ? obd_fw_vtvers_class(resp, s_probe_len)
                                : OBD_VTVERS_UNKNOWN;

    if (ver == OBD_VTVERS_DAMAGED)
    {
        /* the chip runs (it answered ATI) but prints its own error where
           its version was: its identity block is erased, the mark of a
           reset inside its store housekeeping (issue #1023). Nothing here
           repairs that, a firmware rewrite included (13 rewrites on the
           bench changed nothing), so the chip is used as it is and never
           rewritten; its PowerEn setting, lost with the block, is put back
           below. */
        s_note.store_damaged = true;
        obd_chip_version_note("ERR (stored identity damaged)");
        ESP_LOGW(TAG, "the chip runs but its version reads as an error: its "
                 "stored identity is damaged (a reset inside its own store "
                 "housekeeping, issue #1023); it is used as it is and its "
                 "firmware is not rewritten");
    }
    else if (have_ver)
    {
        obd_chip_version_note(resp);

        if (obd_fw_version_is_outdated(resp, obd_chip_builtin_fw_version()))
        {
            /* accepted (left alone by the start-up update) but older than
               the packaged image: the card on System > Maintenance and
               `obd_chip` say the same (Ali, 2026-10-28) */
            ESP_LOGW(TAG, "chip firmware %.*s is older than the packaged %s: "
                     "update it from System > Maintenance or with "
                     "obd_chip update",
                     (int)strcspn(resp, "\r\n>"), resp,
                     obd_chip_builtin_fw_version());
        }
    }

    if (have_ver && (ver == OBD_VTVERS_DAMAGED || obd_fw_version_is_current(resp)))
    {
        uint8_t ppsw = 0;

        resp[0] = '\0';

        if (!obd_bare_probe("VTPPSWS\r", resp, sizeof(resp), 1500) ||
            !obd_parse_ppsw(resp, &ppsw))
        {
            ESP_LOGW(TAG, "PPSW unreadable ('%.12s'): left as it is", resp);
        }
        else if (ppsw != 0x10)
        {
            ESP_LOGW(TAG, "PPSW is %02X, setting to 10", ppsw);
            resp[0] = '\0';

            if (!obd_bare_probe("VTPPSW10\r", resp, sizeof(resp), 1500) ||
                !obd_parse_is_ok(resp))
            {
                ESP_LOGW(TAG, "VTPPSW10 not acknowledged ('%.12s'): no reset, "
                         "the value left as it is", resp);
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(OBD_PPSW_SETTLE_MS));
                resp[0] = '\0';

                if (obd_bare_probe("VTPPSWS\r", resp, sizeof(resp), 1500) &&
                    obd_parse_ppsw(resp, &ppsw) && ppsw == 0x10)
                {
                    ESP_LOGI(TAG, "PPSW reads 10 after the write");
                }
                else
                {
                    ESP_LOGW(TAG, "PPSW reads '%.12s' after the write, not 10",
                             resp);
                }

                /* the reset behind the change, by command (the banner can
                   carry junk bytes ahead of it: sent blind, as the
                   provisioning's `ATZ`, with the chip's boot time) */
                obd_uart_write((const uint8_t *)"ATZ\r", 4);
                vTaskDelay(pdMS_TO_TICKS(1500));
                obd_uart_write((const uint8_t *)"\r", 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                obd_uart_flush_input();
            }
        }
    }

    /* legacy: wait for the READY pin (LOW = ready) before declaring the
       chip up. Legacy forced a reset pulse when it never came; this chip
       has answered every word above, so it gets none (issue #1023) and is
       used as it is. */
    for (int i = 0; !obd_pin_ready() && i < 10; i++)
    {
        ESP_LOGW(TAG, "chip not ready...");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!obd_pin_ready())
    {
        ESP_LOGE(TAG, "the chip answers but READY reads not ready after 2 s: "
                 "used as it is (no reset pulse for a chip that answers)");
    }

    /* echo off first (deterministic responses), then the legacy chip
       provisioning (task §11-5 answered by meatpi: "follow legacy init") */
    obd_bare_probe("ATE0\r", resp, sizeof(resp), 1000);
    obd_bringup_provision();

    /* the stored protocol (2026-11-10): the chip came out of its reset on
       it and nothing of this boot has named a protocol yet (the identity
       and configuration reads above do not), so `ATDPN` here reads the
       store, the only way the chip shows it. One read per bring-up, no
       write: obd_chip_protocol_save() then knows whether a save is needed
       and whether an `ATSP` of an earlier boot, its answer lost, was
       taken. */
    resp[0] = '\0';
    obd_cmd_stored_note(obd_bare_probe("ATDPN\r", resp, sizeof(resp), 1000)
                            ? resp : NULL);

    /* the memory function off (issue #1023, bench 2026-11-01): it is ON
       after every reset (the vendor's pin strap) and then writes every
       protocol switch a request answers on into the chip's EEPROM-backed
       protocol store; legacy's AutoPID sent `atm0` at every start, v6 never
       did. Here, after the provisioning's own ATZ, for every reset this
       sequence performs; the request engine repeats it ahead of every
       protocol switch (obd_chip_cmd.c), so a chip that stays silent here
       is covered at its first switch. */
    bool mem_off = obd_bare_probe("ATM0\r", resp, sizeof(resp), 1000) &&
                   strstr(resp, "OK") != NULL;

    if (mem_off)
    {
        obd_core_memory_off_note();
    }
    else
    {
        ESP_LOGW(TAG, "ATM0 not acknowledged ('%.16s'): the chip may store "
                      "a protocol change until the first switch", resp);
    }

    obd_bare_probe("ATI\r", resp, sizeof(resp), 1500); /* identity for the log */

    esp_err_t err = obd_uart_rx_task_start();

    if (err != ESP_OK)
    {
        return err;
    }

    char stored[OBD_STORED_LEN];

    obd_cmd_stored_text(stored, sizeof(stored));
    ESP_LOGI(TAG, "started: chip '%s' at %d baud (ready=%d, memory %s, "
                  "stored protocol %s)", resp, *active_baud, obd_pin_ready(),
             mem_off ? "off" : "on", (stored[0] != '\0') ? stored : "not read");
    return ESP_OK;
}
