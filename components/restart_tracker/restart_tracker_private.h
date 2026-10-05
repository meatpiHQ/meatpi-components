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
 * @file restart_tracker_private.h
 * @brief Pure state-machine core (restart_tracker_core.c): no IDF deps, so
 *        the host unit tests compile it directly. Time, uptime, and the reset
 *        reason are injected by the caller; restart_tracker.c is the target
 *        glue (locking, PSRAM noinit placement, cache msync, esp_* sources).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "restart_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_MAGIC               0x5254524BU /* "RTRK" */
#define RT_VERSION             1U
#define RT_MIN_VALID_UNIX_TIME 1704067200LL /* 2024-01-01: clock sanity floor */

/* injected boot-time inputs */
typedef struct
{
    int64_t  now_unix;        /**< time(NULL); may be pre-epoch garbage      */
    uint64_t uptime_ms;       /**< since boot                                */
    uint32_t reset_reason;    /**< esp_reset_reason_t value                  */
} rt_inputs_t;

uint32_t rt_crc32(const restart_tracker_state_t *state);
bool     rt_state_is_valid(const restart_tracker_state_t *state);
void     rt_state_reset(restart_tracker_state_t *state);

/** Adopt-or-reset, then append this boot's record (consumes any pending
 *  intent). Returns true if the previous state was invalid and got reset. */
bool rt_record_boot(restart_tracker_state_t *state, const rt_inputs_t *in);

/** Store a planned-restart intent for the next boot. */
void rt_mark_planned(restart_tracker_state_t *state, const rt_inputs_t *in,
                     restart_tracker_planned_reason_t reason,
                     restart_tracker_source_t source, uint32_t flags);

bool rt_reset_reason_is_unexpected(uint32_t reason);

/** Record fields a run fills in after its boot (the history's log of what
 *  the brake keeps in RTC memory). */
void rt_record_set_mode(restart_tracker_state_t *state, uint8_t mode);
void rt_record_set_settled(restart_tracker_state_t *state);

/** CRC-32 (IEEE 802.3) over a byte range: the envelope's and the crash
 *  note's checksum. */
uint32_t rt_crc32_bytes(const uint8_t *data, size_t len);

/* ---- the crash note -----------------------------------------------------------
 * restart_tracker_crash_core.c is the pure half (validity, the filing rules
 * of the next boot, lookup, text); restart_tracker_crash.c holds the store
 * in RTC slow memory and the two hooks around IDF's panic handler. */

#define RT_CRASH_STORE_MAGIC 0x52435354U /* "RCST" */
#define RT_CRASH_NOTE_MAGIC  0x52434E54U /* "RCNT" */
#define RT_CRASH_VERSION     2U /* 2: the brake's count joins the store */

#define RT_CRASH_BT_LEN      RESTART_TRACKER_CRASH_BT_LEN
#define RT_CRASH_BT2_LEN     RESTART_TRACKER_CRASH_BT2_LEN
#define RT_CRASH_ELF_LEN     16 /* CONFIG_APP_RETRIEVE_LEN_ELF_SHA         */
#define RT_CRASH_TASK_LEN    16 /* CONFIG_FREERTOS_MAX_TASK_NAME_LEN       */
#define RT_CRASH_REASON_LEN  32
#define RT_CRASH_TEXT_LEN    96

#define RT_CRASH_HF_PSEUDO     0x01U /* cause is one of IDF's PANIC_RSN_    */

#define RT_CRASH_TF_IN_ISR     0x01U /* crashed inside an interrupt handler */
#define RT_CRASH_TF_NESTED     0x02U /* a second panic inside the handler   */
#define RT_CRASH_TF_EARLY      0x04U /* before restart_tracker_init()       */
#define RT_CRASH_TF_BT_CORRUPT 0x08U /* IDF's `|<-CORRUPTED`                */
#define RT_CRASH_TF_BT_MORE    0x10U /* IDF's `|<-CONTINUES`                */
#define RT_CRASH_TF_TEXT_CUT   0x20U /* the abort text was longer           */

/**
 * One crash, as the panic hooks leave it. Two checksums because the two
 * stages commit separately: a note whose stage B never finished still has a
 * valid head. Fixed-width character fields carry no terminator when full.
 */
typedef struct
{
    /* head: stage A, before IDF prints */
    uint32_t magic;       /**< RT_CRASH_NOTE_MAGIC                           */
    uint32_t sequence;    /**< 0 while pending; the boot it is filed under   */
    uint8_t  kind;        /**< restart_tracker_crash_kind_t                  */
    uint8_t  core;
    uint8_t  head_flags;  /**< RT_CRASH_HF_                                  */
    uint8_t  reserved;
    uint32_t cause;       /**< EXCCAUSE                                      */
    uint32_t pc;          /**< the register, as the register dump prints it  */
    uint32_t excvaddr;
    uint32_t sp;          /**< a1: where the stack walk starts               */
    uint32_t ra;          /**< a0: the caller of the faulting function       */
    uint32_t reason_ptr;  /**< info->reason: the pointer, read in stage B    */
    uint32_t head_crc;    /**< over [magic, head_crc)                        */

    /* tail: stage B, after IDF printed everything */
    uint32_t uptime_s;
    uint8_t  tail_flags;  /**< RT_CRASH_TF_                                  */
    uint8_t  bt_len;
    uint8_t  bt2_len;
    uint8_t  bt2_core;
    char     elf[RT_CRASH_ELF_LEN];
    char     task[RT_CRASH_TASK_LEN];
    char     reason[RT_CRASH_REASON_LEN];
    char     text[RT_CRASH_TEXT_LEN];
    uint32_t bt[RT_CRASH_BT_LEN];
    uint32_t bt2[RT_CRASH_BT2_LEN];
    uint32_t tail_crc;    /**< over [uptime_s, tail_crc)                     */
} rt_crash_note_t;

/* ---- the crash-loop brake -----------------------------------------------------
 * restart_tracker_brake_core.c. Its count sits in the crash store, RTC slow
 * memory: it must not depend on PSRAM, the first thing a broken firmware
 * (or a PSRAM memory test left on) takes away. */

#define RT_REPORT_BUDGET 4U /* crash reports stored until a run settles    */

#define RT_BRAKE_RF_SETTLED 0x01U /* this run was up RESTART_TRACKER_SETTLE_S */
#define RT_BRAKE_RF_RETRY   0x02U /* this (parked) run ends to try again      */

typedef struct
{
    uint8_t  streak;       /**< runs in a row that crashed before settling   */
    uint8_t  parks;        /**< parks since that streak began                */
    uint8_t  run_mode;     /**< restart_tracker_boot_mode_t of the run in
                                progress: the next boot reads it as "before" */
    uint8_t  run_flags;    /**< RT_BRAKE_RF_, same lifetime                   */
    uint8_t  budget;       /**< crash reports that may still be stored       */
    uint8_t  reserved;
    uint16_t test_retry_s; /**< bench: the next park ends after this (once)  */
    uint32_t crc;          /**< over the eight bytes before it               */
} rt_brake_state_t;

/** The store in RTC slow memory: the brake's count, the note the hooks
 *  write, and the notes the boots filed, slot for slot beside the tracker's
 *  history records. */
typedef struct
{
    uint32_t magic;       /**< RT_CRASH_STORE_MAGIC                          */
    uint16_t version;     /**< RT_CRASH_VERSION                              */
    uint16_t note_size;   /**< sizeof(rt_crash_note_t): a layout change
                               resets the store                              */
    rt_brake_state_t brake;  /**< never touched by the panic hooks           */
    rt_crash_note_t pending; /**< written by the panic hooks only            */
    rt_crash_note_t kept[RESTART_TRACKER_HISTORY_LEN]; /**< [i] = history[i] */
} rt_crash_store_t;

_Static_assert(sizeof(rt_crash_note_t) == 300, "crash note layout");
_Static_assert(sizeof(rt_brake_state_t) == 12, "brake state layout");
_Static_assert(sizeof(rt_crash_store_t) ==
                   8 + 12 + 300 * (1 + RESTART_TRACKER_HISTORY_LEN),
               "crash store layout");

/** A reset the brake counts: the panic handler or a watchdog. */
bool     rt_reset_reason_is_crash(uint32_t reason);
uint32_t rt_brake_crc(const rt_brake_state_t *st);
bool     rt_brake_valid(const rt_brake_state_t *st);
void     rt_brake_reset(rt_brake_state_t *st);

/**
 * A boot's step: read how the run before went (`crash_reset`: it ended in
 * a crash that no restart request had announced), move the count, give the
 * verdict, and start the new run's flags. `retry_s` is the product's own
 * park retry (0 = none); the bench's knob, when set, takes its place once.
 */
void rt_brake_boot(rt_brake_state_t *st, bool crash_reset, uint32_t retry_s,
                   restart_tracker_brake_t *out);
void rt_brake_settle(rt_brake_state_t *st);
void rt_brake_set_mode(rt_brake_state_t *st, uint8_t mode);
void rt_brake_mark_retry(rt_brake_state_t *st);
bool rt_brake_spend(rt_brake_state_t *st); /**< one report; false: none left */
void rt_brake_set_test_retry(rt_brake_state_t *st, uint16_t seconds);

uint32_t rt_crash_head_crc(const rt_crash_note_t *note);
uint32_t rt_crash_tail_crc(const rt_crash_note_t *note);
bool     rt_crash_head_valid(const rt_crash_note_t *note);
bool     rt_crash_tail_valid(const rt_crash_note_t *note);
bool     rt_crash_store_valid(const rt_crash_store_t *store);
void     rt_crash_store_reset(rt_crash_store_t *store);

/**
 * The next boot's filing step. The pending note, when it is valid, moves to
 * `kept[slot]` under `sequence` (a tail that never got its checksum is
 * dropped: the note stays, incomplete); with no pending note the slot is
 * cleared. A store that is not ours, or `fresh_history` (the tracker's
 * numbering starts again), clears every kept note first. The pending slot is
 * empty afterwards. Returns the filed note, or NULL.
 */
const rt_crash_note_t *rt_crash_collect(rt_crash_store_t *store,
                                        uint32_t sequence, uint32_t slot,
                                        bool fresh_history);

/** The note filed under `sequence`, or NULL. */
const rt_crash_note_t *rt_crash_find(const rt_crash_store_t *store,
                                     uint32_t sequence);

/** A note for its readers: strings terminated and made printable. */
void rt_crash_export(const rt_crash_note_t *note,
                     restart_tracker_crash_t *out);

/** A fixed-width field that may lack its terminator, as a string: anything
 *  that is not printable ASCII becomes '?'. */
void rt_put_text(char *dst, size_t dst_cap, const char *src, size_t src_len);

/** Target glue (restart_tracker_crash.c, or its stub): file the pending note
 *  under this boot. Called once by restart_tracker_init(). */
const rt_crash_note_t *rt_crash_boot_collect(uint32_t sequence, uint32_t slot,
                                             bool fresh_history);

/** Target glue: the brake's count in the store, valid once
 *  rt_crash_boot_collect() ran. NULL on a target without the store. */
rt_brake_state_t *rt_crash_brake_state(void);

/* ---- the stored crash report -----------------------------------------------------
 * restart_tracker_report_core.c is the pure half (the record, the wear
 * guard, the text); restart_tracker_report.c does the NVS calls. */

#define RT_REPORT_MAGIC     0x52525054U /* "RRPT" */
#define RT_REPORT_VERSION   1U
#define RT_REPORT_FW_LEN    32      /* esp_app_desc_t.version              */
#define RT_REPORT_REFRESH_S 86400   /* the same crash, a day on: its date  */

/** The NVS blob. Fixed-width character fields carry no terminator when
 *  full, as the note's. */
typedef struct
{
    uint32_t magic;       /**< RT_REPORT_MAGIC                               */
    uint16_t version;     /**< RT_REPORT_VERSION                             */
    uint16_t size;        /**< sizeof(rt_report_t): a layout change drops
                               the stored report                             */
    int64_t  stored_unix; /**< 0: the clock was not set                      */
    uint8_t  streak;      /**< the brake's count when it was stored          */
    uint8_t  parked;      /**< 1: the brake parked the device on it          */
    uint8_t  reserved[6];
    char     firmware[RT_REPORT_FW_LEN]; /**< "" unless the image that
                               stored it is the one that crashed             */
    rt_crash_note_t note;
    uint32_t crc;         /**< over [magic, crc)                             */
} rt_report_t;

_Static_assert(sizeof(rt_report_t) == 360, "stored report layout");

typedef enum
{
    RT_REPORT_KEEP = 0,  /**< the stored report already says it              */
    RT_REPORT_NO_BUDGET, /**< news, but the budget is spent                  */
    RT_REPORT_STORE,
} rt_report_decision_t;

uint32_t rt_report_crc(const rt_report_t *report);
bool     rt_report_valid(const rt_report_t *report);

/** A report of `note`. `now_unix` under the clock sanity floor is stored as
 *  0; `firmware` may be NULL or "". */
void rt_report_build(rt_report_t *out, const rt_crash_note_t *note,
                     int64_t now_unix, const char *firmware, uint8_t streak,
                     bool parked);

/** What makes two notes the same crash. */
uint32_t rt_report_identity(const rt_crash_note_t *note);

/** The wear guard: whether `fresh` goes to flash over `stored` (NULL or
 *  not valid: nothing is stored). */
rt_report_decision_t rt_report_decide(const rt_report_t *stored,
                                      const rt_report_t *fresh,
                                      uint8_t budget);

void rt_report_export(const rt_report_t *report, restart_tracker_report_t *out);

/** The report as text. `device` may be NULL. snprintf semantics. */
int rt_report_text(const restart_tracker_report_t *report, const char *device,
                   char *buf, size_t cap);

/** Target glue (restart_tracker_report.c): load the stored report and, when
 *  this boot filed `note`, store it if the wear guard says so. Called once
 *  by restart_tracker_init(), on the boot task. */
void rt_report_boot(const rt_crash_note_t *note,
                    const restart_tracker_record_t *record,
                    const restart_tracker_brake_t *brake);

#ifdef __cplusplus
}
#endif
