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
 * @file obd_chip_private.h
 * @brief Internals shared between the obd_chip sources. The parse section is
 *        PURE (no IDF deps): the host unit tests compile obd_chip_parse.c
 *        directly against it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "obd_chip_guard.h" /* obd_guard_t for the guard accounting hook */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure parsing / framing (obd_chip_parse.c, host-testable) -------------- */

/* 4096 -> 16384 (2026-09-08): a 4 KB ISO-TP response (the chip's maximum,
 * "4K data = 8K ASCII") is 8.2 KB of hex with spaces off and ~12.5 KB with
 * spaces on plus `N:` line prefixes; the engine truncated it at 4096
 * ("response to '23230001' exceeded 4096 bytes"). Two accumulators live in
 * PSRAM (cmd engine + bare_probe), so the cost is 24 KB of PSRAM. */
#define OBD_RESP_MAX 16384

/** Response accumulator: feed RX bytes until the '>' prompt terminates it. */
typedef struct
{
    char   buf[OBD_RESP_MAX];
    size_t len;
    bool   done;     /**< prompt seen                                        */
    bool   overflow; /**< response exceeded OBD_RESP_MAX (kept truncated)    */
} obd_resp_acc_t;

void obd_parse_reset(obd_resp_acc_t *acc);

/**
 * Append @p n bytes; stops consuming at the first '>' (sets done). Returns
 * the number of bytes consumed (== n unless the prompt appeared earlier).
 * Bytes after the prompt belong to the next transaction/stream.
 */
size_t obd_parse_feed(obd_resp_acc_t *acc, const char *data, size_t n);

/**
 * Post-process a finished accumulator into a caller buffer: strip the echo
 * of @p cmd (chips boot with echo on), the terminating prompt, leading/
 * trailing CR/LF and the bytes outside printable ASCII a reset's UART glitch
 * puts ahead of the banner (bench 2026-11-02). Returns the response length.
 */
size_t obd_parse_extract(const obd_resp_acc_t *acc, const char *cmd,
                         char *out, size_t out_len);

/**
 * One programmable parameter out of an `ATPPS` summary ("0C:23 N  0D:0D F",
 * 12 lines of 4): @p value its stored byte, @p on whether the chip applies
 * it (N = on, F = off, the ELM327's own letters). False when @p pp is not
 * in the text. Pure, host-tested (the PP 0F guard of the bring-up).
 */
bool obd_parse_pp(const char *atpps, uint8_t pp, uint8_t *value, bool *on);

/** '?' alone / "UNABLE TO CONNECT" / "CAN ERROR"-style chip errors. */
bool obd_parse_is_chip_error(const char *resp);

/** The chip's acknowledgement and nothing else: `OK` alone, with any CR,
 *  LF or space around it; "NOT OK", "OK?" and a banner are not it (the
 *  sleep word, the PPSW write; 2026-11-07). Pure, host-tested. */
bool obd_parse_is_ok(const char *resp);

/** Case/whitespace-insensitive prefix classification of monitor commands. */
bool obd_parse_is_monitor_cmd(const char *cmd);

/** True when @p cmd selects a protocol: `ATSPh` / `ATTPh` (with the `A`
 *  and `00` forms), STN `STPhh`, vendor `VTPhhh`; spaces and case as the
 *  chip reads them, a ';' ends the word. Not the reads (`STPR`, `STPBR`,
 *  `ATDPN`). The request engine sends `ATM0` ahead of each one (issue
 *  #1023, bench 2026-11-01). Pure, host-tested. */
bool obd_parse_sets_protocol(const char *cmd);

/** An app's reset word as the whole chunk (issue #1023, 2026-11-01): `ATZ`,
 *  `ATWS` or `ATD`, spaces and case as the chip reads them, line ends and
 *  blanks around it allowed, nothing else in the chunk (a chunk carrying a
 *  second command is not one). The bridge path sends ATZ as ATWS and puts
 *  ATM0 behind the answer, hidden. Pure, host-tested. */
typedef enum
{
    OBD_RESET_NONE = 0,
    OBD_RESET_ATZ,
    OBD_RESET_ATWS,
    OBD_RESET_ATD,
} obd_reset_t;

obd_reset_t obd_parse_reset_word(const char *buf, size_t len);

/** True when @p cmd makes the chip transmit on the vehicle bus: hex data, a
 *  bare CR (the chip repeats its last command), the K-line starts (`ATFI`,
 *  `ATSI`), `ATRTR`, STN `STPX`, the vendor's `VTFullyRequestCk`. Every
 *  other AT / ST / VT word only sets or reads the chip. Spaces and case as
 *  the chip reads them, a ';' ends the word. The request engine holds these
 *  back while a protocol switch is owed (2026-11-10). Pure, host-tested
 *  (obd_chip_class.c). */
bool obd_parse_is_bus_request(const char *cmd);

/** The protocol an `ATDPN` answer names, as one of the ELM numbers: `6` ->
 *  '6', `A6` -> '6' with @p automatic set (the chip searches when 6 fails),
 *  `A` -> 'A' (J1939), `AA` -> 'A' automatic; blanks and line ends around it
 *  ignored. False for anything else: an empty answer, `?`, the STN and
 *  vendor presets (`34 [ST]`, `11A [VT]`). Read right after a reset it is
 *  the chip's STORED protocol (2026-11-10). Pure, host-tested
 *  (obd_chip_class.c). */
bool obd_parse_dpn(const char *resp, char *proto, bool *automatic);

/* ---- the K-line session (obd_chip_kline.c, pure, host-tested) ---------------
 * ISO 9141-2 and ISO 14230-4 (protocols 3, 4, 5): the bus is started by the
 * first request (`BUS INIT: ...OK`, 2.6 s with the 5 baud address, bench
 * 2026-11-12) and the ECU ends the session after five seconds without a
 * request. The chip keeps none alive and does not notice one that ended. */

/** How long a session is taken to last without a request: ISO 9141-2's
 *  P3max is 5 s (measured: alive after 4.6 s, gone after 5.0 s). */
#define OBD_KLINE_IDLE_MS 4500

/** What a request that starts the bus is given at least: the chip prints
 *  `OK` or `ERROR` by 2.9 s and the reply 150 ms behind it. */
#define OBD_BUS_START_MS  5000

typedef struct
{
    bool     up;        /**< a bus start ended in `OK`, nothing ended it since */
    char     proto;     /**< the protocol it came up on ('\0' = none named)   */
    char     selected;  /**< what the engine's last protocol word named       */
    int64_t  last_us;   /**< when the bus last answered                       */
    uint32_t starts;    /**< bus starts that ended in `OK`                    */
    uint32_t failed;    /**< ... in `ERROR` (nobody there)                    */
    uint32_t closed;    /**< sessions closed ahead of a request: idled out    */
} obd_kline_t;

/** The answer so far (no prompt yet) says the chip is starting a K-line
 *  bus: `BUS INIT` in it. Any byte sent now ends the start with `STOPPED`,
 *  so the request waits for the chip's own verdict. */
bool obd_parse_bus_starting(const char *text, size_t len);

/** The chip was reset: no session, no protocol named (the counters stay). */
void obd_kline_reset(obd_kline_t *k);

/** A word about to go out: `ATPC` and the reset words end the session, a
 *  switch to another protocol does, the same protocol again does not. */
void obd_kline_word(obd_kline_t *k, const char *cmd);

/** The answer to a bus request: `BUS INIT: ...OK` is a session that came
 *  up, `...ERROR` one that did not, data in a live session moves its
 *  clock. `NO DATA` changes nothing (an unsupported request is answered
 *  with silence on a K-line). */
void obd_kline_reply(obd_kline_t *k, const char *cmd, const char *resp,
                     int64_t now_us);

/** @p cmd is a bus request and the session cannot be alive any more: the
 *  bus last answered longer ago than OBD_KLINE_IDLE_MS. The engine sends
 *  `ATPC` first, so the request starts the bus. Not for `ATSI` / `ATFI`. */
bool obd_kline_stale(const obd_kline_t *k, const char *cmd, int64_t now_us);

/** Milliseconds since the bus last answered in a live session; UINT32_MAX
 *  when no session is up. */
uint32_t obd_kline_idle_ms(const obd_kline_t *k, int64_t now_us);

/* ---- STSLCS sleep-config (obd_chip_stslcs.c, pure, host-testable) -----------
 * Ported from legacy main/obd.c: the sscanf patterns are the accepted
 * chip-output grammar. */

typedef struct
{
    char ctrl_mode[10]; /* "ELM327" or "NATIVE"                          */
    int  pwr_ctrl;      /* 1 = HIGH, 0 = LOW                             */
    struct { int en; uint32_t time; } uart_sleep;
    struct { int en; uint32_t min_time; uint32_t max_time; } uart_wake;
    int  ext_input_level;
    struct { int en; int level; uint32_t time; } ext_sleep;
    struct { int en; int level; uint32_t time; } ext_wake;
    struct { int en; float voltage; uint32_t time; } vl_sleep;
    struct { int en; float voltage; uint32_t time; } vl_wake;
    struct { int en; float voltage_change; uint32_t time; } vchg_wake;
} obd_stslcs_t;

/** Parse a full STSLCS response (any line endings) into @p out. */
void obd_stslcs_parse(const char *response, obd_stslcs_t *out);

/** `VTPPSWS`, the chip's PowerEn switch, as its two hex digits (`10`: low
 *  sleeps, high wakes; `0F`: no function): the first line that is exactly
 *  two hex digits, an echo or the stray bytes of a reset ahead of it
 *  skipped. False when no such line. Pure, host-tested (2026-11-07). */
bool obd_parse_ppsw(const char *resp, uint8_t *value);

/** True when the chip's stored sleep config must be rewritten: any of its
 *  autonomous controls (UART or voltage, sleep or wake) is ON. The stored
 *  thresholds and times are not compared since 2026-11-07: they are inert
 *  while the controls are off (`test.ps1 sleeptrig`). */
bool obd_stslcs_needs_provision(const obd_stslcs_t *c);

/* Firmware-file line iterator (pure): yields trimmed non-empty lines. */
typedef struct
{
    const char *cur;
    const char *end;
} obd_fw_iter_t;

void   obd_fw_iter_init(obd_fw_iter_t *it, const char *data, size_t len);
size_t obd_fw_iter_next(obd_fw_iter_t *it, char *line, size_t line_len);
bool   obd_fw_line_is_end_marker(const char *line); /* "FFF1..." */

/** True when @p vtvers (the chip's VTVERS answer) names a chip firmware
 *  the packaged image leaves alone: the packaged version and the ones it
 *  is known to coexist with (legacy v4.52p, main fab54ad: V2.3.22 and
 *  V2.3.24). Pure, host-tested (issue #1023, 2026-10-28). */
bool        obd_fw_version_is_current(const char *vtvers);
const char *obd_fw_current_versions(void); /* "V2.3.22, V2.3.24", for logs */
/* The version numbers (2026-11-01, Ali: no forced rewrite; a chip is
   written only when the image is newer). `parse` finds the first
   "V<major>.<minor>.<patch>" in a VTVERS answer, the packaged constant or
   a file's path; `cmp` orders two parsed versions field by field; `token`
   writes the version alone ("V2.3.24", for the page and the API; 0 and ""
   when there is none); `is_outdated` = the chip's version is below the
   image's (false for silence, '?' or an image without a version). Pure,
   host-tested. */
bool        obd_fw_version_parse(const char *s, unsigned out[3]);
int         obd_fw_version_cmp(const unsigned a[3], const unsigned b[3]);
size_t      obd_fw_version_token(const char *s, char *out, size_t out_len);
bool        obd_fw_version_is_outdated(const char *vtvers, const char *image);

/** Lines in an update image (its records): the progress denominator. */
uint32_t    obd_fw_count_records(const char *buf, size_t len);

/** What a chip that answers `ATI` says to `VTVERS` (issue #1023): its
 *  version; nothing this firmware knows (`?` from a chip without the word or
 *  in its download mode, an empty answer); or its own error text (`ERR82`
 *  behind a few stray bytes), which is what a chip prints whose identity
 *  block was erased by a reset inside its store housekeeping. Such a chip
 *  runs, and a firmware rewrite does not repair it. @p len: the answer's
 *  length (it can hold a NUL). Pure, host-tested. */
typedef enum
{
    OBD_VTVERS_VERSION = 0,
    OBD_VTVERS_UNKNOWN,
    OBD_VTVERS_DAMAGED,
} obd_vtvers_t;

obd_vtvers_t obd_fw_vtvers_class(const char *resp, size_t len);

/** True when the last update run streamed an image and finalized it (the
 *  chip restarted: the caller brings it up again). */
bool obd_fw_last_flashed(void);

/** True while the update JOB (obd_chip_fw_job.c) runs: the stream and the
 *  fresh bring-up after it. obd_chip_update_in_progress() folds it in so
 *  the sleep entry waits for the whole job (Ali, 2026-11-08). */
bool obd_fw_job_running(void);

/* ---- READY settled (obd_chip_settle.c, pure, host-tested; issue #1023) -------
 * The chip tidies its store during a boot, and a reset inside that tidying
 * destroys the store. No bring-up decides anything until READY has held one
 * level long enough that no boot can be running, and the reset line is the
 * last resort for a chip that answers nothing (obd_settle_pulse_allowed). */

/** READY ready without a break for this long = the chip is in its main
 *  loop. Longer than the 117 to 123 ms a waking chip reads ready BEFORE it
 *  restarts itself (measured; 100 ms would pass there). */
#define OBD_READY_STEADY_MS 300

/** READY not ready without a break for this long = asleep or stuck, no boot
 *  is running. A boot holds it not ready for about 265 ms, about 450 ms
 *  with the store to tidy. */
#define OBD_DOWN_STEADY_MS  1000

/** How long the bring-up waits for READY to settle behind the sleep pin (a
 *  wake settles at about 800 ms, at about 980 ms with the store to tidy),
 *  and how long a reset pulse waits for it before it is refused. */
#define OBD_WAKE_SETTLE_MS  2500
#define OBD_PULSE_SETTLE_MS 2500

typedef enum
{
    OBD_SETTLE_MOVING = 0, /* READY changed too recently: a boot may run     */
    OBD_SETTLE_READY,      /* ready for OBD_READY_STEADY_MS without a break  */
    OBD_SETTLE_DOWN,       /* not ready for OBD_DOWN_STEADY_MS without one   */
} obd_settle_verdict_t;

typedef struct
{
    bool    seen;     /* a sample was fed                                   */
    bool    ready;    /* the level of the last sample                       */
    int64_t since_us; /* when READY took that level                         */
} obd_settle_t;

void obd_settle_reset(obd_settle_t *s);

/** One READY sample, @p ready at @p now_us: the verdict so far. Feed it at
 *  least every few milliseconds; a level change restarts the count. */
obd_settle_verdict_t obd_settle_feed(obd_settle_t *s, bool ready,
                                     int64_t now_us);

/** THE RULE OF THE RESET LINE (issue #1023; Ali, 2026-11-12: "the pin reset
 *  should not be allowed unless as a last resort"), pure. A pulse may go
 *  out only when both hold: @p chip_silent, the caller has just asked the
 *  chip at both UART speeds and got no prompt at either; and READY is at
 *  rest, ready for OBD_READY_STEADY_MS or not ready for OBD_DOWN_STEADY_MS
 *  (no boot of the chip lasts that long). Never while READY moves, and
 *  never for a chip that was not asked first: READY reading "asleep" is not
 *  enough. A chip that answers is reset by command, which it runs from its
 *  own main loop. The pulse itself is declared in obd_chip_reset_line.h,
 *  out of reach of everything but the bring-up. */
bool obd_settle_pulse_allowed(obd_settle_verdict_t settled, bool chip_silent);

/* ---- UART layer (obd_chip_uart.c, target only) ------------------------------ */

esp_err_t obd_uart_init(int baud);
esp_err_t obd_uart_set_baud(int baud);
int       obd_uart_get_baud(void); /* the baud set last: what the UART runs at */
esp_err_t obd_uart_write(const uint8_t *data, size_t len);
int       obd_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
void      obd_uart_flush_input(void);

/** The wire's counters since boot (obd_chip_stats_t carries them on). */
typedef struct
{
    uint32_t tx_bytes;
    uint32_t rx_overflows;
    uint32_t rx_buffered;  /* in the driver's ring now                      */
    uint32_t rx_handovers; /* ISR passes that found the hardware FIFO at or
                              above the hand-over threshold                 */
    uint32_t rx_late;      /* of those, 40 us or more behind their interrupt */
    uint16_t rx_fifo_peak; /* the fullest any pass found the FIFO           */
} obd_uart_stats_t;

void      obd_uart_get_stats(obd_uart_stats_t *out);
esp_err_t obd_uart_rx_task_start(void);
void      obd_uart_rx_task_stop(void);

/* pins (obd_chip_pins.c) */
void obd_pins_init(void);
void obd_pin_wake(void);       /* hold-release sequence, sleep pin high      */
void obd_pin_sleep(void);      /* low + pulldown + hold (survives resets)     */
bool obd_pin_ready(void);

/** Wait until READY has settled (obd_chip_settle.c), at most @p limit_ms:
 *  READY, DOWN, or MOVING when it never held still. @p waited_ms (may be
 *  NULL): how long that took. Polls every 2 ms; needs the scheduler. */
obd_settle_verdict_t obd_pin_settle(uint32_t limit_ms, uint32_t *waited_ms);

/* The reset pulse is NOT declared here on purpose (obd_chip_reset_line.h,
 * included by the bring-up alone): no other file of the component can send
 * one, and nothing outside the component ever could since 2026-11-12. */

/** The reset line since boot: pulses sent (0 on a healthy chip, whatever
 *  the device did), and pulses asked for and refused. */
typedef struct
{
    uint32_t pulses;
    uint32_t refused;
} obd_pin_stats_t;

void obd_pin_get_stats(obd_pin_stats_t *out);

/* ---- core bridges ------------------------------------------------------------ */

/** Fan-out entry point called by the RX task with each chunk. */
void obd_core_fanout(const uint8_t *data, size_t len);

/** True while EXCLUSIVE is held (RX task pauses fan-out; TX rejects). */
bool obd_core_exclusive_held(void);

/** The claim held now: 0 none, 1 command, 2 monitor, 3 exclusive
 *  (obd_chip_claim.c; the stats name it). */
int obd_core_claim_state(void);

/* Async bring-up gate (2026-07-26): start() launches the wake/negotiate/
 * provision sequence on a one-shot task; wire-touching APIs wait here so
 * nothing interleaves with the bare-UART probes. Returns true when
 * bring-up has finished (chip up OR given up) or never launched
 * (degraded start: legacy pass-through behavior); false only if it is
 * still in flight after timeout_ms. */
#define OBD_BRINGUP_WAIT_MS 25000 /* > worst case: a chip that reads ready
                                     and is silent (warm start, the full
                                     2-baud x 3-retry walk, the last-resort
                                     pulse, one more look: 18.6 s measured
                                     2026-11-12) or one found only then,
                                     plus its provisioning; 20 s until the
                                     walk came ahead of the pulse */
bool obd_core_bringup_wait(uint32_t timeout_ms);

/* The sleep word (2026-11-07, 4.x parity): `STSLEEP0` ahead of the PowerEn
 * pin in obd_chip_sleep(true); the request's budget. The chip answers at
 * once and sleeps on its own a second or two later (READY follows that
 * late: bench 2026-11-07, still awake 1.5 s after the OK), so the answer
 * is the verdict and the first nap's READY check the ground truth. */
#define OBD_SLEEP_WORD_MS   1000

/* obd_chip_soft_reset(): the budget of each of its words, and its tries. */
#define OBD_SOFT_RESET_MS    1000
#define OBD_SOFT_RESET_TRIES 2

/* `VTPPSW10` at the bring-up (the one EEPROM write of that path): legacy's
 * 100 + 500 ms before any reset; the value is read back inside that window
 * and only then the reset, an `ATZ` since 2026-11-12 (2026-11-07). */
#define OBD_PPSW_SETTLE_MS  600

/** The boot sequence on the bare UART (obd_chip_bringup.c): wake, reset
 *  (verified), baud negotiation, provisioning, RX task start. Called by the
 *  bring-up task in obd_chip.c only. @p active_baud is written as the
 *  sequence learns it. */
esp_err_t obd_bringup_run(int *active_baud);

/** One request and its answer on the bare UART (obd_chip_bringup.c; no RX
 *  task reads the wire: the bring-up, or a holder of the exclusive claim).
 *  True when a prompt arrived inside @p timeout_ms; @p resp is the answer
 *  without the echo. Not reentrant. */
bool obd_bare_probe(const char *cmd, char *resp, size_t resp_len,
                    uint32_t timeout_ms);

/** The legacy provisioning of the chip's sleep control (obd_chip_provision.c;
 *  the bring-up calls it on the bare UART, the echo off). */
void obd_bringup_provision(void);

/** Behind a chip firmware update (obd_chip_fw.c, under its exclusive
 *  claim): the chip restarts itself, so it gets the reset line only when it
 *  answers at neither UART speed. The UART is left at the speed it had. */
void obd_bringup_reset_if_silent(void);

/** What the last bring-up met (obd_chip_bringup.c; GET /api/obd_chip
 *  `reset` and `fw.damaged`; issue #1023). */
typedef struct
{
    uint32_t wake_ms;       /* READY settled this long behind the sleep pin  */
    bool     wake_ready;    /* ... as ready (false: asleep, stuck, moving)   */
    bool     silent;        /* the chip answered at neither UART speed: the
                               last resort was asked for                     */
    bool     wake_pulsed;   /* ... and the reset pulse went out (not refused
                               for a READY that never came to rest)          */
    bool     store_damaged; /* the chip runs, its identity reads as an error */
} obd_bringup_note_t;

void obd_bringup_note(obd_bringup_note_t *out);

/* ---- fault codes (obd_chip_fault.c, pure, host-tested; 2026-11-12) -----------
 * What a bring-up leaves behind in dev_status_manager's fault store, until
 * it is cleared by hand (`faults -c`, POST /api/faults/clear). */

/** The chip answered at neither UART speed: the last resort was taken. */
#define OBD_FAULT_SILENT         "obd_chip_silent"
/** The chip runs but its stored identity reads as an error (issue #1023). */
#define OBD_FAULT_DAMAGED        "obd_chip_damaged"
#define OBD_FAULT_DAMAGED_DETAIL "VTVERS reads an error: stored identity erased"

/** The detail of OBD_FAULT_SILENT for a bring-up that found the chip
 *  @p silent (obd_bringup_note_t), sent the reset pulse or not (@p pulsed)
 *  and ended with the chip up or not (@p found). False = no fault: the chip
 *  answered. @p detail holds up to 47 characters (dev_status_fault_t). */
bool obd_fault_silent(bool silent, bool pulsed, bool found, char *detail,
                      size_t detail_len);

/** The chip of the last bring-up runs but its stored identity is damaged:
 *  no firmware rewrite, automatic or asked for (it repairs nothing, and the
 *  start-up check used to repeat it for ever). */
bool obd_bringup_store_damaged(void);

/* claim internals shared by request engine + fw update */
esp_err_t obd_core_claim(int type, uint32_t timeout_ms);
void      obd_core_release(void);

/** True when the calling task holds a transaction (obd_chip_txn_begin):
 *  the request engine's ATM0 + switch pair then rides the caller's hold
 *  instead of taking one of its own. */
bool      obd_core_txn_owned(void);

/** EEPROM guard accounting + log line (GET /api/obd_chip
 *  `eeprom_guard`): called by every TX path that ran the guard with a
 *  non-PASS verdict; @p cmd/@p len is the text as the caller received it. */
void obd_core_guard_note(obd_guard_t v, const char *cmd, size_t len);

/** One `ATM0` the driver sent and the chip acknowledged (the bring-up's,
 *  or the request engine's ahead of a protocol switch): GET /api/obd_chip
 *  `eeprom_guard.memory_off` (issue #1023, 2026-11-01). */
void obd_core_memory_off_note(void);

/** One PP 0F the bring-up found off or not 95 and wrote back (Ali,
 *  2026-11-02): GET /api/obd_chip `eeprom_guard.pp_restores`. */
void obd_core_pp_restore_note(void);

/** The fan-out's and the raw TX path's counters (obd_chip_fanout.c), for
 *  obd_chip_get_stats(): the RX accounting, the EEPROM guard, the ATM0
 *  words, the app resets intercepted, the bytes hidden behind them and
 *  the PP 0F restores. */
typedef struct
{
    uint32_t rx_bytes;
    uint32_t rx_chunks;
    uint16_t rx_max_chunk;
    uint32_t guard_rewrites;
    uint32_t guard_blocked;
    uint32_t memory_off;
    uint32_t app_resets;
    uint32_t hidden_bytes;
    uint32_t pp_restores;
} obd_fanout_stats_t;

void obd_core_fanout_stats(obd_fanout_stats_t *out);

/** obd_gate owner identity for the MIC chip (address only, defined in
 *  obd_chip.c, shared with the request engine in obd_chip_cmd.c). */
extern const char obd_chip_gate_owner[1];

/** Chip UART speed: FIXED, deliberately not a setting (meatpi 2026-09-07:
 *  exposing it invited a user changing it, breaking the chip and claiming
 *  warranty). The chip powers on at 115200 until `STWBR` persists this as
 *  its default; bring-up walks 2 M -> 115200 and switches (`STSBR`). */
#define OBD_CHIP_BAUD 2000000

/** The chip's factory power-on baud: the bring-up's fallback, and the
 *  other baud the firmware update gate probes (issue #1023). */
#define OBD_CHIP_FALLBACK_BAUD 115200

/* config applied at boot by the settings descriptor */
typedef struct
{
    bool auto_sleep;
    bool monitor_auto_interrupt;
    bool auto_update;       /* flash the packaged fw on version mismatch
                               after bring-up (legacy parity, default on) */
    float    wake_voltage;  /* chip VL wake threshold (V)               */
    float    sleep_voltage; /* chip VL sleep threshold (V)              */
    uint32_t sleep_time_s;  /* chip VL sleep hold; minutes*60+30 legacy */
} obd_config_t;

const obd_config_t *obd_settings_config(void);
bool obd_settings_is_configured(void);
esp_err_t obd_settings_register(void);

/* request engine (obd_chip_cmd.c) */
esp_err_t obd_cmd_engine_init(void);
uint32_t  obd_cmd_protocol_saves(void); /* obd_chip_protocol_save() count */

/** The request path's memory protection (2026-11-10): the protocol switches
 *  the engine refused because the chip did not acknowledge the `ATM0` ahead
 *  of them, the bus requests held back since, and whether a switch is still
 *  owed (GET /api/obd_chip `eeprom_guard`). */
typedef struct
{
    uint32_t refused;
    uint32_t held;
    bool     owed;
} obd_cmd_memory_t;

void obd_cmd_memory_stats(obd_cmd_memory_t *out);

/** A fresh chip (the bring-up finished): nothing is owed any more, every
 *  holder sends its own prelude again. */
void obd_cmd_memory_reset(void);

/** The chip's stored protocol as the bring-up read it (`ATDPN` right after
 *  the chip's reset, before anything of this boot names a protocol); NULL
 *  when the chip did not answer. obd_chip_protocol_save() writes only when
 *  the chip holds something else (2026-11-10). */
void obd_cmd_stored_note(const char *dpn);

/** The save's side of the stats: the saves that were not needed because
 *  the chip held the protocol already, and the stored protocol in the
 *  chip's words ("" = not known: never read, or an `ATSP` went out and its
 *  answer was lost). */
#define OBD_STORED_LEN 12
uint32_t obd_cmd_protocol_kept(void);
void     obd_cmd_stored_text(char *out, size_t out_len);

/** The engine's K-line session and its counters (obd_chip_kline.c), a copy. */
void obd_cmd_kline_stats(obd_kline_t *out);

#ifdef OBD_CHIP_HOST_TEST
/** Host tests only (standard §3): the per-boot state of the save as a
 *  restart leaves it. */
void obd_cmd_test_new_boot(void);
/** Host tests only: the K-line session as it is @p ms later with nothing
 *  asked meanwhile. */
void obd_cmd_test_kline_age(uint32_t ms);
#endif

/* fw update helper (obd_chip_fw.c): VTVERS with update-mode semantics */
esp_err_t obd_chip_get_fw_version_raw(char *buf, size_t buf_len);

/** Remember the chip's VTVERS answer (the bring-up and the update gate
 *  call it): GET /api/obd_chip shows it without a wire request. */
void obd_chip_version_note(const char *vtvers);

#ifdef __cplusplus
}
#endif

/* event_manager glue (obd_chip_events.c) */
void oc_events_register(void);
