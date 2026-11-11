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
 * @file autopid_private.h
 * @brief Internal types + pure-module contracts shared by the autopid
 *        .c files and the host suite. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pool bounds (generous by design: meatpi 2026-07-06: PSRAM is
        plentiful; caps exist for static allocation, not rationing) -------- */
#define AP_MAX_GROUPS   32
#define AP_MAX_PIDS     512
#define AP_MAX_FILTERS  128
#define AP_MAX_PARAMS   2048   /* pooled across all PIDs + filters          */
#define AP_PARAMS_PER   256    /* per PID/filter: published vehicle
                                  profiles decode up to 192 values from ONE
                                  DID (Xpeng cell voltages; Hyundai/Kia BMS
                                  DIDs carry 20 to 32), so 16 rejected every
                                  Hyundai/Kia/Genesis/Xpeng profile
                                  (2026-09-16). Sizes the poller's PSRAM
                                  copy + the filter frame slots, never a
                                  stack frame.                              */

#define AP_NAME_LEN     32
#define AP_CMD_LEN      24
#define AP_INIT_LEN     96
#define AP_HDR_LEN      16
#define AP_EXPR_LEN     96
#define AP_MUX_EXPR_LEN 48     /* raw multiplexer-switch slice              */
#define AP_UNIT_LEN     16
#define AP_CLASS_LEN    24

#define AP_PAYLOAD_MAX  128    /* framing-free payload bytes: the parsers
                                  (VIN, DTC, detection), the cross-talk guard */
#define AP_FRAMES_MAX   256    /* the reply as the chip prints it with headers
                                  on, what user expressions index (2026-10-24,
                                  issue #1014): 28 frames of 8 (the Leaf
                                  ZE1's 2102, the Xpeng's 221122) = 224      */
#define AP_RESP_MAX     2048   /* raw chip response text: 28 frames behind a
                                  29-bit id are ~1040 characters              */
#define AP_RESTORE_LEN  128    /* the baseline prelude + every undo command
                                  (ap_chain_restore: 107 chars at most)     */

typedef enum
{
    AP_PID_STD = 0,
    AP_PID_CUSTOM,
    AP_PID_SPECIFIC,
} ap_pid_type_t;

typedef struct
{
    char   name[AP_NAME_LEN];
    char   expression[AP_EXPR_LEN];
    char   mux_expr[AP_MUX_EXPR_LEN]; /* "" = unconditional; else raw
                                         switch slice that must equal
                                         mux_val for expression to apply */
    char   unit[AP_UNIT_LEN];
    char   class[AP_CLASS_LEN];
    bool   enabled;
    float  min;                /* plausibility clamp; NAN = none            */
    float  max;
    float  mux_val;
} ap_param_t;

typedef struct
{
    char          name[AP_NAME_LEN];
    ap_pid_type_t type;
    char          cmd[AP_CMD_LEN];
    char          init[AP_INIT_LEN];      /* per-PID extra init, ';'-sep    */
    char          rxheader[AP_HDR_LEN];   /* -> ATCRA filter when set       */
    int           group;                  /* index into groups              */
    uint32_t      period_ms;              /* 0 = inherit group              */
    bool          enabled;
    uint16_t      param_start;            /* slice of the param pool        */
    uint16_t      param_count;
    /* a J1939 parameter group (autopid_j1939.h): cmd "PGN:..", read at
       config load; served from the listener's store, never the chip */
    bool          j1939;
    bool          j1939_request;          /* '?': sent on request (phase 6) */
    int16_t       j1939_sa;               /* pinned source, -1 = the pick   */
    uint32_t      pgn;
    uint8_t       bus;                    /* ap_row_bus_t (autopid_bus.h):
                                             0 = the car's OBD protocol     */
    uint8_t       bus_rate;               /* that bus's bit rate code, 0 =
                                             its own (ap_bus_rate_parse)    */
} ap_pid_t;

/** The scheduling class of a row (autopid_j1939.h has the words). */
typedef enum
{
    AP_CLASS_CHIP = 0,     /* a request through the OBD chip (filters too) */
    AP_CLASS_PASSIVE,      /* a PGN row: read from the listener's store    */
    AP_CLASS_BUS_TX,       /* a PGN row with '?': a request on the native
                              bus (phase 6; read like passive until then)  */
    AP_CLASS_CHIP_BUS,     /* a request through the chip on a bus of its own
                              (MS, single-wire, CH CAN: autopid_bus.h); the
                              HS bus guard has no say about it             */
} ap_row_class_t;

#define AP_CLASS_BIT(c)   (1u << (c))
#define AP_CLASS_ALL      (AP_CLASS_BIT(AP_CLASS_CHIP) | \
                           AP_CLASS_BIT(AP_CLASS_PASSIVE) | \
                           AP_CLASS_BIT(AP_CLASS_BUS_TX) | \
                           AP_CLASS_BIT(AP_CLASS_CHIP_BUS))

/** Is this the class of a row the chip asks (on any bus)? */
#define AP_CLASS_IS_CHIP(c) ((c) == AP_CLASS_CHIP || (c) == AP_CLASS_CHIP_BUS)

typedef struct
{
    uint32_t frame_id;
    bool     is_extended;
    uint32_t monitor_ms;                  /* ATMA window length             */
    int      group;
    uint32_t period_ms;                   /* 0 = inherit group              */
    bool     enabled;
    uint16_t param_start;
    uint16_t param_count;
} ap_filter_t;

typedef struct
{
    char     name[AP_NAME_LEN];
    bool     enabled_default;
    uint32_t period_ms;
} ap_group_t;

/** The loaded config tables (static PSRAM pools; autopid_config.c fills). */
typedef struct
{
    ap_group_t  groups[AP_MAX_GROUPS];
    ap_pid_t    pids[AP_MAX_PIDS];
    ap_filter_t filters[AP_MAX_FILTERS];
    ap_param_t  params[AP_MAX_PARAMS];
    uint16_t    n_groups;
    uint16_t    n_pids;
    uint16_t    n_filters;
    uint16_t    n_params;
} ap_config_t;

/* ---- pure: scheduler (autopid_sched.c, host-tested) ----------------------
 * The PID is the scheduling unit; entries cover pids then filters
 * (entry index i: i < n_pids -> pid i; else filter i - n_pids).          */

typedef struct
{
    int64_t  due_us;
    int64_t  last_run_us;      /* round-robin tiebreak for period 0        */
    uint16_t fail_streak;
} ap_sched_slot_t;

typedef struct
{
    ap_sched_slot_t slots[AP_MAX_PIDS + AP_MAX_FILTERS];
    /* runtime (EPHEMERAL) group state: autopid_group_set() */
    bool    group_enabled[AP_MAX_GROUPS];
    int32_t group_period_override[AP_MAX_GROUPS]; /* <0 = none             */
    /* per-type enables from settings knobs */
    bool    type_enabled[3];
    /* the type's init carries a word the OBD chip stores (autopid_bus.h):
       its rows on the car's protocol are not asked while it does */
    bool    type_held[3];
} ap_sched_t;

#define AP_SCHED_BACKOFF_STREAK 3  /* fails before backing off             */
#define AP_SCHED_BACKOFF_MULT   4
#define AP_SCHED_STAGGER_US     10000

/** Measured chip+ECU round-trip floor (Phase 1b bench: 53 ms/request,
 *  ~19 req/s ceiling). Configured periods 1..floor-1 ms are accepted but
 *  flagged in /api/autopid; period 0 = deliberate max-rate mode. */
#define AP_PERIOD_FLOOR_MS      50

/** Yield-to-app window: the poller stays off the chip until an external
 *  ELM client (TCP/BLE/USB/WS app) has been silent this long. Legacy
 *  parity: main.c cleared DEV_AUTOPID_ELM327_APP_BIT on every app
 *  command and a 10 s timer set it back. Bench 2026-09-08 with the two
 *  interleaved: 80 of 200 app requests answered with autopid's lines,
 *  STOPPED or NO DATA (the "choppy RPM dial" report). */
#define AP_CLIENT_YIELD_MS      10000

/** True while the poller must stay off the chip for an external client
 *  (@p idle_ms = ms since the client's last write, UINT32_MAX = never). */
bool ap_sched_client_hold(uint32_t idle_ms);

/** Reset slots (stagger initial due times) + runtime state from config. */
void ap_sched_reset(ap_sched_t *st, const ap_config_t *cfg, int64_t now_us);

/** Effective period of entry @p i (µs; group inheritance + runtime
 *  override + fail backoff). 0 = high-fidelity. */
int64_t ap_sched_period_us(const ap_sched_t *st, const ap_config_t *cfg,
                           int i);

/** Next runnable entry: smallest due (ties -> least recently run).
 *  @return entry index or -1 when nothing is enabled.
 *  @param[out] due_us when to run it (may be in the past = now). */
int ap_sched_next(const ap_sched_t *st, const ap_config_t *cfg,
                  int64_t *due_us);

/** ap_sched_next over the entries of the given classes only (a bit set
 *  of AP_CLASS_BIT(ap_row_class_t); autopid_j1939.h): the poller asks for
 *  the chip's rows only while it may use the chip, for the passive rows
 *  always. */
int ap_sched_next_of(const ap_sched_t *st, const ap_config_t *cfg,
                     unsigned classes, int64_t *due_us);

/** The scheduling class of entry @p i (filters are chip rows). */
ap_row_class_t ap_sched_entry_class(const ap_config_t *cfg, int i);

/** Record a run of entry @p i: reschedule + fail-streak bookkeeping. */
/** Runtime group control (pure): set = the §5b override (period < 0 keeps
 *  the current override), restore = back to the configured defaults (the
 *  undo of a while-rule). @p g is a valid group index. */
void ap_sched_group_set(ap_sched_t *st, int g, bool enabled,
                        int32_t period_override_ms);
void ap_sched_group_restore(ap_sched_t *st, const ap_config_t *cfg, int g);

void ap_sched_ran(ap_sched_t *st, const ap_config_t *cfg, int i,
                  int64_t now_us, bool ok);

/** True when entry @p i is enabled (entry + group + type gates, and not a
 *  row of the car's protocol behind a held type init). */
bool ap_sched_entry_enabled(const ap_sched_t *st, const ap_config_t *cfg,
                            int i);

/* ---- cache (autopid_cache.c: target; slot index == param pool index) ----- */
#ifndef AUTOPID_HOST_TEST
#include "cJSON.h"
void  ap_cache_init(void);
void  ap_cache_clear(void);
void  ap_cache_put(uint16_t slot, double value, int64_t ts_us);
bool  ap_cache_get(uint16_t slot, double *value, int64_t *ts_us);
cJSON *ap_cache_snapshot(const ap_config_t *cfg);
cJSON *ap_cache_detail(const ap_config_t *cfg);
int   ap_cache_find(const ap_config_t *cfg, const char *param);

/* External (injected) values: named samples that are NOT config-slot
 * bound (GPS from the ESPNetLink dongle). Merged into snapshot/get so
 * they ride the same autopid_data push, ${autopid.*} pulls, value sink,
 * and autopid.param events as polled parameters. */
int   ap_ext_put(const char *name, const char *unit, double value,
                 int64_t ts_us, bool *out_changed);
bool  ap_ext_get(const char *name, double *value, int64_t *ts_us);

/* config file IO (autopid_config.c, target half) */
esp_err_t autopid_config_load(ap_config_t *cfg);
esp_err_t autopid_config_save(const char *json, size_t len);
const char *autopid_config_path(void);

/* core internals shared with the http/cli surfaces (autopid.c) */
const ap_config_t *ap_core_config(void);
/* autopid_group.c (runtime group control + its JSON) borrows the core's
   lock, scheduler state and poller wake-up through these */
void ap_core_lock(void);
void ap_core_unlock(void);
/* the tables were replaced since the last call (taken once, under the
   core's lock, by the poller): the row indexes mean other rows now */
bool ap_core_tables_new(void);
ap_sched_t *ap_core_sched(void);
void ap_core_wake(void);
esp_err_t ap_core_group_json(cJSON *arr);   /* append group states       */
uint16_t ap_core_sub_floor_count(void);     /* PIDs configured 1..49 ms  */
void ap_core_scan_pause(bool on);           /* std scan owns the chip    */

/* settings (autopid_settings.c: standard §4.1) */
esp_err_t ap_settings_register(void);       /* the "autopid" descriptor  */
bool ap_settings_is_configured(void);       /* boot apply ran (§4.3)     */
int  ap_settings_pause_below_mv(void);      /* 0 = never pause           */
bool ap_settings_pause_follow_sleep(void);  /* legacy parity: pause
                                               requests below sleep_mv   */
bool ap_settings_pause_all(void);           /* pause_mode "all": the
                                               voltage pause stops the
                                               J1939 rows too            */
const char *ap_core_std_protocol(void);     /* std_protocol setting      */
const char *ap_core_specific_init_default(void); /* specific_init knob  */
uint32_t ap_core_min_event_interval_ms(void);

/* on_apply -> runtime state owned by autopid.c */
void ap_core_set_enabled(bool enabled);
void ap_core_set_type_enabled(int type, bool enabled);
void ap_core_set_type_held(int type, bool held); /* ap_sched_t.type_held */

/* event_manager glue (autopid_events.c: Phase 3) */
void ap_events_register(void);              /* sources/action/values     */
void ap_events_reset(void);                 /* clear emission memory     */
void ap_events_param(const ap_param_t *prm, uint16_t slot, int group,
                     double value);         /* on-change + min interval  */
void ap_events_external(const char *name, const char *unit, double value,
                        bool changed);      /* injected value → sink+event */
void ap_events_pid_failed(const char *name, uint16_t streak);
void ap_events_scan_done(uint16_t found);
void ap_events_vehicle_changed(const char *vin, const char *name, bool known);
void ap_events_vehicle_evicted(const char *vin, const char *name); /* store */

/* chip-facing runner (autopid_runner.c: poller-task context; the baseline
   half, prelude / restore / send_init, in autopid_baseline.c) */
bool ap_runner_run(const ap_pid_t *pid, int pid_index,
                   const ap_param_t *params);
/** The publish half of a poll, shared with the J1939 runner: every enabled
 *  parameter of @p pid evaluated over @p payload (mux precondition,
 *  expression, plausibility clamp) into the cache and the events, stamped
 *  @p ts_us (the reply's time: now for a chip answer, the message's own
 *  time for a J1939 group read from the store).
 *  @return true when at least one value was published. */
bool ap_runner_publish(const ap_pid_t *pid, const ap_param_t *params,
                       const uint8_t *payload, size_t payload_len,
                       int64_t ts_us);
/** The poller has nothing to ask for @p wait_ms and a K-line session is up:
 *  is it time for the one short request that keeps the ECU from ending it
 *  (ap_proto_kline_keep)? Then ap_runner_kline_keep() sends it. Poller-task
 *  context. */
bool ap_runner_kline_keep_due(uint32_t wait_ms);
void ap_runner_kline_keep(void);
void ap_runner_set_type_init(int type, const char *init);
/** The word of the type's init that the OBD chip stores (a protocol preset,
 *  a link word; autopid_bus.h), as the chip reads it: the init is held, its
 *  rows on the car's protocol are not asked. "" = not held. */
const char *ap_runner_type_held(int type);
/** The setting a type's init comes from (`std_init`, `custom_init`,
 *  `specific_init`), for a message. */
const char *ap_runner_type_init_name(int type);
void ap_runner_reset(void);        /* replay inits on the next poll     */
/** After an external ELM app had the chip: re-send the protocol prelude
 *  (spaces on, headers off, timeout, protocol, header, mask) and replay
 *  every type/PID init on the next poll. Poller-task context. */
void ap_runner_restore_baseline(void);

/** The bus guard found the stored protocol at the wrong bitrate for this
 *  bus: the prelude uses the chip's search (ATTP0) for the rest of the
 *  boot, exactly like the silent-protocol fallback. Any task. */
void ap_runner_force_search(void);

/* bus guard glue (autopid_guard.c): what the native controller heard on
   the bus decides whether the chip may be pinned to a CAN protocol */
void ap_guard_init(void);
/** Poller, before every transmission: may the chip be used now? Final
 *  once the bus named its bitrate or an ECU answered; until then a
 *  listen-only watch stays on the bus (the first call waits 400 ms). */
bool ap_guard_poll_ok(void);
/** Poller: an ECU answered on the protocol in effect (its bitrate is the
 *  bus's): the verdict is final, the watch is let go. */
void ap_guard_proven(void);
/** A one-shot chip job is about to start (detection, DTC scan / clear,
 *  test-a-PID): false = nothing may be transmitted; @p reason says why. */
bool ap_guard_job_ok(char *reason, size_t cap);
/** Whoever is about to send a prelude that pins @p proto (the runner's
 *  baseline, its restore): does the standing verdict cover THAT protocol?
 *  false = do not send; the poller's next ap_guard_poll_ok() looks at the
 *  bus for it. Plain reads, any task. */
bool ap_guard_pin_ok(char proto);
/** A chain of the tables is about to go to the chip as written (@p owner
 *  names it for the sentence: "the init of row X", "dtc_init"): may it, on
 *  the bus as the guard last saw it (ap_guard_chain_allowed)? false = send
 *  nothing of it nor of the request behind it; counted and explained in the
 *  status (`bus_guard.refused`, `refused_reason`), and for a job (@p job)
 *  in ap_guard_last_reason(). @p last as in ap_guard_chain_allowed(). */
bool ap_guard_chain_ok(const char *chain, const char *owner, bool job,
                       char *last);
void ap_guard_rearm(void);              /* the protocol in effect changed  */
bool ap_guard_parked(void);
void ap_guard_last_reason(char *out, size_t cap); /* why the last job was
                                                     refused            */
void ap_guard_status_json(cJSON *obj);  /* adds "bus_guard": {...}         */

/* ATMA filter window (autopid_filter.c: poller-task context) */
bool ap_runner_run_filter(const ap_filter_t *f, const ap_param_t *params);

/** One-shot test-a-PID through the real runner choreography (§11):
 *  type init chain, per-PID init, ATCRA, request, ATCRA off, exactly
 *  what a poll of that PID sends (@p type: AP_PID_*, -1 = unknown; it
 *  decides the borrowed-header rule). Caller pauses the poller around it
 *  (ap_core_scan_pause). `transcript` (may be NULL) receives one line
 *  per exchange, "> cmd" then "< reply", so the UI can show what went
 *  out and what came back (2026-09-16). ESP_ERR_NOT_ALLOWED = a chain of
 *  the row sets a protocol the bus guard refuses on this bus: nothing of
 *  the row was sent (ap_guard_last_reason() says why). @p bus: where the
 *  row asks (AP_BUS_KEY, autopid_bus.h; 0 = the car's OBD protocol); on
 *  another bus the chip is taken there, asked, and taken home again, and
 *  @p type_init is not sent. */
esp_err_t ap_runner_test(int type, uint16_t bus, const char *type_init,
                         const char *init, const char *rxheader,
                         const char *cmd, char *raw, size_t raw_len,
                         int64_t *elapsed_us, char *transcript,
                         size_t transcript_len);

/** Tell the reply parser how to read a 29-bit id for the row about to be
 *  parsed (ap_resp_set_id29_hint): the row's receive filter (@p rxheader,
 *  else an ATCRA in @p init, else one in @p type_init) and whether the
 *  protocol in effect is 29-bit. Before ap_resp_frames() of a poll or a
 *  test shot. */
void ap_runner_reply_hint(const char *type_init, const char *init,
                          const char *rxheader);

/* the transcript of a test (autopid_transcript.c, pure): replies flattened
   to one line and capped, a line that does not fit is dropped */
typedef struct
{
    char  *buf;                 /* NULL = no transcript wanted           */
    size_t cap;
    size_t len;
} ap_tr_t;

void ap_tr_add(ap_tr_t *t, char dir, const char *s);

/* chip-state memory shared with first contact (autopid_contact.c,
   poller-task context) */
void ap_runner_send_init(const char *init);      /* a ';'-separated chain  */
void ap_runner_send_prelude(const char *prelude); /* ... that resets the
                                                    header: nothing borrowed,
                                                    and what the tables moved
                                                    put back behind it       */
/** A one-shot job of the firmware's own that talks to the chip directly (the
 *  detection job) starts here: whatever the chains of the tables set beyond
 *  the prelude (a row's own flow control, CAN priority, extended addressing)
 *  is put back, once. Bench 2026-11-10: with only the rows of a profile
 *  live, the row's `ATFCSH` / `ATFCSM1` stayed in effect under the job, the
 *  VIN's reply stopped after its first frame and the detection failed with
 *  "no ECU response". The caller holds the chip job and the poller is
 *  parked (the scan pause). */
void ap_runner_undo_tables(void);
/** Such a job sent a chain of the tables itself (the profile's inits in the
 *  detection's probe): remembered like a row's, undone before the next row
 *  type or job. */
void ap_runner_note_moved(const char *chain);
/** The boot prelude, once. false = it is due and the bus guard has no
 *  verdict for the protocol it would pin: nothing was sent, do not send a
 *  request on top (the next poll asks again). */
bool ap_runner_baseline_ensure(void);
void ap_runner_baseline_invalidate(void);        /* ... is due again       */
/** The init chain the poller sends when it switches to this PID type
 *  (what autopid_settings composed from the protocol + *_init). */
const char *ap_runner_type_init(int type);

/* standard-PID scan (autopid_std.c, target half) */
esp_err_t autopid_std_scan_start(void);     /* INVALID_STATE if running  */
cJSON *ap_std_scan_status_json(void);       /* {status,found,error,ts}   */
const char *ap_std_prelude(void);           /* ATS1;ATH0;ATST96;ATTP<p>.. */
cJSON *ap_std_table_json(void);             /* the full SAE table for UI */
const char *autopid_std_scan_path(void);    /* /data/autopid/std_scan.json */
/* the job's profile probe: ap_scan_probe_t and its API sit after the
   autopid_vehicle.h include at the end of this header (they use its sizes) */
/* one table row -> a config row: ap_std_entry_to_json(), autopid_dialect.h */

/* runner-side identity hooks: see autopid_vehicle.h (included below) */
#endif


/* ---- pure: standard-PID helpers (autopid_std.c, host-tested) -------------
 * ap_std_expression maps a legacy table row (bit_start counts within the
 * headers-on buffer [PCI, mode, PID, A, B, ...]) to a v6 expression over
 * our payload (echo included, no PCI): byte = bit_start/8 - 1.
 * Placeholder rows (bit_length 0) return ESP_ERR_NOT_SUPPORTED.        */
esp_err_t ap_std_expression(uint8_t bit_start, uint8_t bit_length,
                            double scale, double offset, char *buf,
                            size_t buf_len);

/** The same mapping for a dialect whose data sits @p shift bytes further
 *  into the payload (ap_dialect_data_shift: `62 F4 0C A B`). */
esp_err_t ap_std_expression_shift(uint8_t bit_start, uint8_t bit_length,
                                  double scale, double offset, int shift,
                                  char *buf, size_t buf_len);

/** The mapping with the row's range (@p min / @p max, NAN = none): when
 *  max is the value at full scale within float noise the scale is written
 *  as the ratio of two integers (`B2*100/255`), so a full-scale raw decodes
 *  to max EXACTLY (issue #1004: the table's float scale printed as
 *  0.3921568692 decoded 0xFF to 100.000001646, above the max of 100); a
 *  range below zero with no offset reads the raw as two's complement
 *  (`[S2:S3]*0.25`). */
esp_err_t ap_std_expression_bounded(uint8_t bit_start, uint8_t bit_length,
                                    double scale, double offset, double min,
                                    double max, int shift, char *buf,
                                    size_t buf_len);

/** OR-merge every "41 <pid> A B C D" line of @p resp (headers on or off,
 *  multi-ECU) into @p bitmap. True when at least one row matched. PURE. */
bool ap_std_scan_parse(const char *resp, uint8_t expect_pid,
                       uint32_t *bitmap);

/* config parse (PURE half of autopid_config.c) */
esp_err_t ap_config_parse(const char *json, ap_config_t *cfg, char *err,
                          size_t err_len);

#include "cJSON.h" /* the name helpers hand cJSON trees around */
/* ---- parameter names made unique (autopid_names.c, PURE, 2026-10-06) ----
 * Names key the cache and the API; the parser refuses a repeat. The scan's
 * rows are made unique before they are stored and a stored file with a
 * repeat is repaired at load (the later name gets _2, _3, ...).         */
/** Rename every repeated parameter name in a "pids" array (config rows or
 *  the scan's `supported` entries). @return how many were renamed. */
int ap_names_dedupe(cJSON *pids);
/** A config text with repeats -> the repaired text (malloc'd, caller
 *  frees) and the count; NULL and 0 when nothing needed renaming or the
 *  text does not parse (the parser then says what is wrong). */
char *ap_config_repair_names(const char *json, int *renamed);

/** The standard rows of a config text re-numbered to the table's current
 *  expressions (autopid_std.c; 2026-10-24, issue #1014: the expressions
 *  count the frame as the chip prints it, and a car the store knows keeps
 *  its rows across detections, so the file is brought up to date where it
 *  is loaded). A row is matched by its `cmd` (`01XX`, `22F4XX`) and each
 *  parameter by the table's name (a `_2` suffix of the repair allowed);
 *  anything else is left alone. @return the fixed text (malloc'd, caller
 *  frees) and how many expressions changed; NULL and 0 when none did or
 *  the text does not parse. */
char *ap_config_refresh_std(const char *json, int *changed);
int ap_std_refresh_rows(cJSON *pids);

/** In-place neutralization of EEPROM-writing user init commands
 *  (case-insensitive, whitespace-tolerant: "atsp6", "AT SP 6"):
 *  ATSP -> ATTP (same protocol switch, RAM only) and ATM1 -> ATM0
 *  (memory-on would make the chip store every subsequent protocol
 *  change). Init strings replay on every type/PID transition, so
 *  letting either through would wear the chip's EEPROM out (legacy
 *  semantics). PURE. Since 2026-09-16 a thin wrapper over the chip
 *  driver's guard (obd_chip_guard.h), which also names the commands
 *  with no RAM twin (ATPP/ATSD/ATCV/STWBR): config parse refuses those. */
void ap_init_sanitize(char *str);

/* ---- pure: a parameter's min / max applied to a decoded value
 * (autopid_bounds.c, host-tested). Issue #1004: a full tank decoded to
 * 100.000001646 against a max of 100 and was dropped. Float noise never
 * drops a reading; beyond the tolerance a standard OBD row clamps (the
 * table's range is its display range, legacy parity), every other row
 * drops (the bounds of a custom, specific or J1939 row are a filter). ---- */
#define AP_BOUNDS_TOL_REL 1e-6   /* of the bound's magnitude: 100 -> 1e-4   */
#define AP_BOUNDS_TOL_ABS 1e-6   /* the floor, for the bounds around zero    */

typedef enum
{
    AP_BOUNDS_KEPT = 0,   /* inside the bounds: the value as decoded       */
    AP_BOUNDS_CLAMPED,    /* beyond a bound, set to it                     */
    AP_BOUNDS_DROPPED,    /* beyond a bound and the row filters: not cached */
} ap_bounds_verdict_t;

/** Apply @p min / @p max (NAN = none) to @p value: within the tolerance of
 *  a bound the value is clamped to it; beyond it, clamped when
 *  @p clamp_beyond (a standard OBD row), dropped otherwise. A NaN value is
 *  dropped. */
ap_bounds_verdict_t ap_bounds_apply(double *value, float min, float max,
                                    bool clamp_beyond);

#include "autopid_chain_private.h" /* what a chain of the tables sets   */
#include "autopid_guard_private.h" /* the bus guard's pure half          */
#include "autopid_reply_private.h" /* the chip's response text to bytes  */
#include "autopid_dtc_private.h" /* DTC codecs, report, engine, database */
#include "autopid_dbc_private.h" /* DBC codec + store                    */

#ifndef AUTOPID_HOST_TEST
/** One-shot chip-job serialization (std scan / test-a-PID / dtc jobs
 *  never interleave; each still pauses the poller via
 *  ap_core_scan_pause). autopid.c owns the flag. */
bool ap_core_job_acquire(void);
void ap_core_job_release(void);
#endif

#include "autopid_vehicle.h" /* vehicle identity: pure core + file owner */

#ifndef AUTOPID_HOST_TEST
/* the job's profile probe (autopid_scan_probe.c, 2026-10-26): what the
   Quick Setup's "Which car is this?" sends with POST .../vehicles/detect.
   Armed by the httpd task before the start, consumed once by the job.
   Target only: ap_veh_seen_t is the store's target half. */
#define AP_PROBE_CMDS_MAX 3
#define AP_PROBE_CMD_LEN  16
typedef struct
{
    bool    armed;
    char    profile[AP_VEH_PROFILE_LEN];     /* the published name        */
    char    name[AP_VEH_NAME_LEN];           /* the entry's name, "" = the
                                                default                   */
    char    protocol;                        /* the init's ATSP, '\0' none */
    char    init[AP_VEH_INIT_LEN];           /* the profile's init chain  */
    char    cmds[AP_PROBE_CMDS_MAX][AP_PROBE_CMD_LEN]; /* its requests   */
    char    cmd_inits[AP_PROBE_CMDS_MAX][AP_VEH_INIT_LEN]; /* a request's
                                                own init (a profile that
                                                addresses one ECU per
                                                request), "" = none      */
    uint8_t n_cmds;
} ap_scan_probe_t;
void ap_scan_probe_set(const ap_scan_probe_t *p);   /* NULL = clear      */
void ap_scan_probe_clear(void);
const ap_scan_probe_t *ap_scan_probe(void);
/** Scan task: the prelude for the probe's protocol, the init chain, then
 *  the requests until one answers positively. True = confirmed: @p seen
 *  carries the responders (bitmap 0), the protocol, the dialect obd2 and
 *  confirmed_by profile; @p answered the request that did. The bus guard is
 *  asked for every protocol the profile sets, like a row's (2026-11-10);
 *  @p refused = it kept something of the profile off the bus. */
bool ap_scan_probe_run(char *resp, size_t resp_len, ap_veh_seen_t *seen,
                       char *answered, size_t answered_cap, bool *refused);
#endif /* AUTOPID_HOST_TEST */
#include "autopid_dialect.h" /* OBD dialects: requests, parsers, addressing */
#include "autopid_j1939.h"   /* J1939 rows: grammar, classes, the std table */
#include "autopid_proto.h"   /* the vehicle protocols as one table           */

#ifdef __cplusplus
}
#endif

