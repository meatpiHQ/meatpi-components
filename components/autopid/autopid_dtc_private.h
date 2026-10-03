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
 * @file autopid_dtc_private.h
 * @brief The DTC part of autopid_private.h: the pure codecs (DTC, freeze
 *        frame, DTC database), the scan report and the engine / store /
 *        event declarations. A FRAGMENT: included by autopid_private.h
 *        only (it needs its base types); split out 2026-10-02 to keep
 *        both headers under the 700-line rule.
 */
#pragma once

#ifndef AP_NAME_LEN
#error "include autopid_private.h, not this fragment"
#endif

/* ---- pure: DTC codec (autopid_dtc_codec.c — host-tested; TASK_dtc.md §4) --
 * Mode 04 clears EVERYTHING (codes + readiness + MIL) — OBD2 has no
 * per-code clear, so "clear specific DTCs" is a CONDITION on the whole
 * present set (always / if_any / if_only).                              */

#define AP_DTC_MAX       32     /* codes kept per category               */
#define AP_DTC_CODE_LEN  16     /* "SPN524287-31" + NUL: a WWH-OBD
                                   vehicle may report in the SAE J1939
                                   format (was 10 / "P0420-08" until
                                   2026-10-03, 6 / "P0420" until
                                   2026-07-22; FTB 0 omits the suffix so
                                   OBD- and UDS-sourced codes stay
                                   string-identical)                    */
#define AP_DTC_DB_CODE_LEN 10   /* a database keys on the 5-character
                                   J2012 code: its index keeps the old
                                   width (up to 20000 entries)          */

typedef enum
{
    AP_DTC_CLEAR_INVALID = 0,
    AP_DTC_CLEAR_ALWAYS,
    AP_DTC_CLEAR_IF_ANY,        /* >=1 listed code present               */
    AP_DTC_CLEAR_IF_ONLY,       /* every present code is listed          */
} ap_dtc_clear_mode_t;

/** 2-byte DTC -> "P0420" (bits 15-14 letter, 13-12 first digit). */
void ap_dtc_format(uint8_t hi, uint8_t lo, char out[AP_DTC_CODE_LEN]);

/** "P0420" -> 2 bytes; false on malformed input. Case-insensitive. */
bool ap_dtc_unformat(const char *code, uint8_t *hi, uint8_t *lo);

/** Parse a 43/47/4A payload (ap_resp_to_payload output, service echo
 *  first) into formatted codes. Skips 0x0000 padding pairs, tolerates a
 *  stripped count byte, keeps what fits in @p out_max.
 *  @return code count, or -1 when the service byte doesn't match. */
int ap_dtc_parse_codes(const uint8_t *payload, size_t len,
                       uint8_t service_resp,
                       char out[][AP_DTC_CODE_LEN], size_t out_max);

/** Union-merge @p src into @p dst (skip duplicates, keep order).
 *  @return the new dst count (<= dst_max). */
uint8_t ap_dtc_merge_codes(char dst[][AP_DTC_CODE_LEN], uint8_t n_dst,
                           size_t dst_max,
                           const char src[][AP_DTC_CODE_LEN],
                           uint8_t n_src);

/** Parse "41 01 AA .." -> MIL bit (A7) + stored count (A6..A0). */
bool ap_dtc_parse_mil(const uint8_t *payload, size_t len, bool *mil,
                      uint8_t *count);

/** Map a mode string (+ code list, for the default) to the enum;
 *  NULL/"" mode = always without codes, if_any with them. */
ap_dtc_clear_mode_t ap_dtc_clear_mode_parse(const char *mode,
                                            const char *codes);

/** Evaluate the clear condition against the present stored codes. */
bool ap_dtc_clear_allowed(const char present[][AP_DTC_CODE_LEN],
                          size_t n_present, const char *codes,
                          ap_dtc_clear_mode_t mode);

/* ---- pure: freeze-frame codec (TASK_dtc §14 — OBD mode 02, frame 0) ------
 * Payload shapes are ap_resp_to_payloads output with the echo kept:
 * `42 <pid> <frame> <data…>`. Decode rides the standard-PID table
 * (autopid_std.c), byte semantics identical to the mode-01 expressions. */

#define AP_FRZ_MAX      24      /* decoded values kept per report        */
#define AP_FRZ_NAME_LEN 40      /* std-table param names                 */

typedef struct
{
    char  name[AP_FRZ_NAME_LEN];
    char  unit[AP_UNIT_LEN];
    float value;
} ap_frz_val_t;

/** Parse a `42 02 <frame> hi lo` payload -> the DTC that froze the frame
 *  (DTCFRZF). @return true only for a non-zero DTC (0x0000 = no frame
 *  stored — some ECUs answer zeros instead of NO DATA). */
bool ap_frz_dtc(const uint8_t *payload, size_t len,
                char out[AP_DTC_CODE_LEN]);

/** Parse a `42 <base> <frame> b0..b3` supported-PID bitmap payload for
 *  bitmap base @p pid (0x00/0x20/0x40…). MSB of b0 = PID base+1. */
bool ap_frz_bitmap(const uint8_t *payload, size_t len, uint8_t pid,
                   uint32_t *bitmap);

/** Decode a mode-02 value payload `[0x42, pid, frame, A, B, …]` via the
 *  standard-PID table (autopid_std.c owns the table; same byte indexing
 *  as ap_std_expression, value = raw*scale+offset). Appends decoded
 *  params to @p out starting at @p n. @return the new count (<= max). */
int ap_frz_decode(const uint8_t *payload, size_t len, ap_frz_val_t *out,
                  int n, int max);

/* ---- pure: DTC-database importer (autopid_dtc_db_codec.c — host-tested;
 * TASK_dtc_db.md §2). Canonical stored form: "#dtcdb1 <n>\n" then sorted
 * "CODE\tDESC\n" lines.                                                 */

#define AP_DTC_DB_MAX        8      /* databases                        */
#define AP_DTC_DB_NAME_LEN   25     /* cert-set-style names             */
#define AP_DTC_DB_FILE_MAX   (1024 * 1024)  /* upload cap               */
#define AP_DTC_DB_ENTRIES_MAX 20000
#define AP_DTC_DESC_MAX      95     /* description chars kept           */

typedef struct
{
    char     code[AP_DTC_DB_CODE_LEN];
    uint32_t off;                   /* desc offset into scratch/buffer  */
    uint32_t seq;                   /* original order (dedup tiebreak)  */
    uint16_t len;                   /* desc length                      */
} ap_dtc_db_item_t;

/** Sniff + parse @p in (CSV/TSV/;-CSV/JSON-map/JSON-array/plain text)
 *  into scratch + a SORTED deduped item index (last duplicate wins).
 *  CONTRACT: @p scratch_cap must be >= in_len + AP_DTC_DESC_MAX + 16 —
 *  the emitter checks per-entry headroom BEFORE writing (a smaller cap
 *  silently rejects the tail; bench-bitten 2026-07-08).
 *  @return entry count, or -1 (err filled; fmt_out = sniffed format). */
int ap_dtc_db_import(const char *in, size_t in_len, char *scratch,
                     size_t scratch_cap, ap_dtc_db_item_t *items,
                     size_t items_cap, char fmt_out[12], char *err,
                     size_t err_len);

/** Emit the canonical form. @return bytes written, 0 = out too small. */
size_t ap_dtc_db_serialize(const char *scratch,
                           const ap_dtc_db_item_t *items, int n,
                           char *out, size_t out_cap);

/** Index a canonical buffer (validates header + count).
 *  @return entry count or -1. Items' off/len point into @p buf. */
int ap_dtc_db_index(const char *buf, size_t len,
                    ap_dtc_db_item_t *items, size_t items_cap);

/** Picker query: code-prefix OR description-substring, both
 *  case-insensitive; empty/NULL q matches everything. */
bool ap_dtc_db_match(const char *code, const char *desc, size_t desc_len,
                     const char *q);

/* Who reported what. The category arrays of the report are the merged
 * view (one code once, whoever reported it): events, rules, scripts and
 * the clear conditions read those. The items keep every (code, category,
 * ECU) apart with what the service said about it. */
#define AP_DTC_ITEMS_MAX 48
#define AP_DTC_SRC_MAX   8      /* responders kept (= AP_RESP_ECUS_MAX)   */

typedef enum
{
    AP_DTC_KIND_STORED = 0,     /* confirmed                             */
    AP_DTC_KIND_PENDING,
    AP_DTC_KIND_PERMANENT,
} ap_dtc_kind_t;

typedef struct
{
    char     code[AP_DTC_CODE_LEN];
    uint32_t ecu;               /* responder CAN id; UINT32_MAX = not
                                   told apart (headers were off)         */
    uint8_t  kind;              /* ap_dtc_kind_t                         */
    uint8_t  status;            /* ISO 14229 statusOfDTC; 0 = the service
                                   carries none (OBD-II 03 / 07 / 0A)    */
    uint8_t  severity;          /* WWH-OBD severity and class; 0 = none  */
    bool     j1939;             /* a DM1 / DM2 code: `ecu` is the source
                                   address, `oc` its occurrence count;
                                   kind pending = previously active (DM2) */
    uint8_t  oc;                /* J1939 occurrence count, 127 = n/a     */
} ap_dtc_item_t;

/* J1939 lamp bits (DM1 byte 1) as the report keeps them */
#define AP_DTC_LAMP_MIL 0x01    /* malfunction indicator                 */
#define AP_DTC_LAMP_RSL 0x02    /* red stop                              */
#define AP_DTC_LAMP_AWL 0x04    /* amber warning                         */
#define AP_DTC_LAMP_PL  0x08    /* protect                               */

typedef struct
{
    uint32_t ecu;               /* responder CAN id                      */
    bool     mil;               /* this ECU asks for the lamp            */
    uint8_t  count;             /* the confirmed-code count it reports   */
    bool     j1939;             /* a J1939 controller: `ecu` is its
                                   source address                        */
    uint8_t  lamps;             /* AP_DTC_LAMP_* it asks for             */
} ap_dtc_src_t;

/** The scan report (RAM-only; guarded by the dtc module's own lock).
 *  About 5 KB: it lives in PSRAM statics and is never copied onto a task
 *  stack (ap_dtc_report_json and the small accessors read it in place). */
typedef struct
{
    int64_t  ts_us;             /* completion time (esp_timer clock)     */
    int64_t  ts_epoch;          /* wall clock, 0 when time isn't valid   */
    bool     valid;
    bool     mil;               /* any ECU asks for the lamp             */
    uint8_t  mil_count;         /* summed across responding ECUs         */
    uint8_t  n_ecus;            /* responders (1 when headers off)       */
    char     protocol[8];       /* "obd" | "uds" | "wwh" | "j1939": the
                                   path that answered (the chip's word
                                   when a J1939 vehicle has one too)     */
    bool     j1939;             /* DM1 of a J1939 network folded in      */
    uint8_t  lamps;             /* AP_DTC_LAMP_* any controller asks for */
    uint8_t  n_stored, n_pending, n_permanent, n_new;
    char     stored[AP_DTC_MAX][AP_DTC_CODE_LEN];
    char     pending[AP_DTC_MAX][AP_DTC_CODE_LEN];
    char     permanent[AP_DTC_MAX][AP_DTC_CODE_LEN];
    char     new_codes[AP_DTC_MAX][AP_DTC_CODE_LEN];
    uint8_t  n_items;
    ap_dtc_item_t items[AP_DTC_ITEMS_MAX];
    uint8_t  n_src;
    ap_dtc_src_t src[AP_DTC_SRC_MAX];
    /* freeze frame (mode 02 frame 0, OBD scans only — TASK_dtc §14) */
    bool     frz_present;
    char     frz_dtc[AP_DTC_CODE_LEN];  /* DTCFRZF                       */
    uint32_t frz_ecu;           /* responder CAN id; UINT32_MAX = hdr off */
    uint8_t  n_frz;
    ap_frz_val_t frz[AP_FRZ_MAX];
    char     error[48];         /* last scan error, "" = ok              */
} ap_dtc_report_t;

/* ---- pure: filling a report (autopid_dtc_codec.c — host-tested) ---------- */

/** One code of @p kind as @p ecu reported it: merged into the category
 *  array (once per code) and kept as an item (once per ECU).
 *  @return false when the code is empty or its category array is full
 *  (the code is dropped; a full item table only costs the detail). */
bool ap_dtc_report_add(ap_dtc_report_t *r, ap_dtc_kind_t kind,
                       const char *code, uint32_t ecu, uint8_t status,
                       uint8_t severity);

/** One responder's lamp word (its `0101` / `22F401` answer): kept per
 *  ECU, and folded into `mil` (any), `mil_count` (the sum, saturating)
 *  and `n_ecus`. A responder seen twice keeps its latest word. */
void ap_dtc_report_src(ap_dtc_report_t *r, uint32_t ecu, bool mil,
                       uint8_t count);

/** "stored" / "pending" / "permanent". */
const char *ap_dtc_kind_name(ap_dtc_kind_t kind);

/** One code of a J1939 controller (`SPN110-0`, source address @p sa,
 *  occurrence count @p oc): active (DM1) = a stored code, @p previous
 *  (DM2, previously active) = a pending one, and an item that says so.
 *  @return as ap_dtc_report_add. */
bool ap_dtc_report_add_j1939(ap_dtc_report_t *r, const char *code,
                             uint8_t sa, uint8_t oc, bool previous);

/** One J1939 controller's lamp word (DM1 byte 1 as AP_DTC_LAMP_* bits,
 *  @p count active codes): kept per source, its MIL bit folded into `mil`
 *  as a responder's lamp word is, every bit into `lamps`. */
void ap_dtc_report_src_j1939(ap_dtc_report_t *r, uint8_t sa, uint8_t lamps,
                             uint8_t count);

#ifndef AUTOPID_HOST_TEST
/* DTC engine (autopid_dtc.c — target half; TASK_dtc.md §5) */
void ap_dtc_init(void);                      /* autopid_init context     */
void ap_dtc_apply_settings(const cJSON *settings); /* on_apply context   */
bool ap_dtc_enabled(void);
bool ap_dtc_clear_gate_open(void);           /* dtc_allow_clear          */
/** The path a scan or clear takes NOW: "obd" (services 03 / 07 / 0A, clear
 *  04), "wwh" (19 42 / 19 55, clear 14 FFFF33: the current car's dialect is
 *  uds) or "uds" (`dtc_protocol` = uds: 19 02 on one address pair). */
const char *ap_dtc_path(void);
bool ap_dtc_busy(void);
esp_err_t ap_dtc_scan_start(void);           /* 409-equiv INVALID_STATE  */
void ap_dtc_periodic_check(void);            /* poller loop: start scan
                                                when the period is due   */
/** The last report in a nutshell (any pointer may be NULL). */
void ap_dtc_report_brief(bool *valid, bool *mil, uint8_t *mil_count,
                         uint8_t *n_stored);
/** The stored codes as "P0420,P0171" (what fits @p out_len). */
void ap_dtc_report_stored_csv(char *out, size_t out_len);
cJSON *ap_dtc_report_json(void);             /* the §8 GET "report" obj  */
/** ... plus `desc` {code: text} from the DTC databases (the HTTP view). */
cJSON *ap_dtc_report_json_desc(void);

/** Conditional clear (sync, seconds of bus I/O — NEVER the event
 *  dispatcher; HTTP/CLI/scan-job/script contexts only). Re-reads mode 03
 *  first, evaluates, sends 04, confirms with a second 03.
 *  @param[out] cleared condition held + 44 confirmed
 *  @param[out] before/after stored-code counts around the clear. */
esp_err_t ap_dtc_clear(const char *codes, const char *mode, bool *cleared,
                       uint8_t *before, uint8_t *after, char *err,
                       size_t err_len);

/** Fire-and-forget clear for the event action (dispatcher context must
 *  not block on the bus) — spawns the job task, result travels by the
 *  autopid.dtc_clear event. */
esp_err_t ap_dtc_clear_queue(const char *codes, const char *mode);

/* DTC-database store + PSRAM cache (autopid_dtc_db.c — TASK_dtc_db §3) */
void ap_dtc_db_init(void);                   /* autopid_init context     */
void ap_dtc_db_load_all(void);               /* internal-stack ONLY (fs) */
esp_err_t ap_dtc_db_store(const char *name, const char *raw,
                          size_t raw_len, int *entries_out,
                          char fmt_out[12], char *err, size_t err_len);
esp_err_t ap_dtc_db_delete(const char *name);
bool ap_dtc_db_lookup(const char *code, char *out, size_t out_cap);
cJSON *ap_dtc_db_list_json(void);
cJSON *ap_dtc_db_search_json(const char *q, const char *db_name,
                             int offset, int limit);
void ap_dtc_db_desc_map(cJSON *parent, const char *key,
                        const ap_dtc_report_t *r);

/* DTC event glue (autopid_events.c) */
void ap_events_dtc_register(void);           /* sources+actions+values   */
void ap_events_dtc_new_code(const char *code, const char *status,
                            bool mil);
void ap_events_dtc_scan(const ap_dtc_report_t *r, bool ok);
void ap_events_dtc_clear(bool ok, bool cleared, uint8_t before,
                         uint8_t after);
#endif
