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
 * @file data_logger_private.h
 * @brief Internals shared between the lifecycle/writer, the storage
 *        engines, the pure helpers, and the glue files.
 *
 * Two record STREAMS since 2026-07-09 (TASK addendum): numeric params
 * (`dl_<epoch>.<ext>`) and raw CAN frames (`can_<epoch>.<ext>`), each
 * with its own engine choice, rotation and retention. Engines take a
 * context pointer so two files can be open at once.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "data_logger.h"

#define DL_MAX_PARAMS   256  /* PSRAM registry; a real autopid profile
                                carries hundreds of params */
#define DL_SOURCE_MAX   16
#define DL_NAME_MAX     32
#define DL_RING_LEN     2048  /* param ring (PSRAM)                     */
#define DL_CAN_RING_MAX 8192 /* frame ring cap; ring_len setting picks
                                the live size (static PSRAM, ~192 KB
                                at cap — caps not rationing)           */
#define DL_DIR          "/sd/logs"
#define DL_PREFIX_PARAM "dl_"
#define DL_PREFIX_CAN   "can_"
/* ROBUSTNESS.md (2026-09-07) */
#define DL_CORRUPT_SUFFIX ".corrupt"            /* set-aside file: <name>.corrupt   */
#define DL_CORRUPT_KEEP   2                     /* set-aside files kept per stream  */
#define DL_QUICKCHECK_MAX (8u * 1024u * 1024u)  /* resume-time quick_check bound (B) */
#define DL_BYTES_PER_ROW   26 /* measured: 100 KB / 4000 rows          */
#define DL_BYTES_PER_FRAME 44 /* sqlite frames-table size estimate     */

/* record kinds */
#define DL_REC_PARAM 0
#define DL_REC_FRAME 1

/* frame flag bits (dl_record_t.u.f.flags and the .wdl 0x03 id word) */
#define DL_FRAME_EXT 0x01
#define DL_FRAME_RTR 0x02

/* one queued record (PSRAM rings; 24 bytes) */
typedef struct
{
    int64_t ts_ms;
    union
    {
        struct
        {
            double  value;
            int16_t param;
        } p;
        struct
        {
            uint32_t id;
            uint8_t  data[8];
            uint8_t  dlc;
            uint8_t  flags;      /* DL_FRAME_EXT / DL_FRAME_RTR       */
        } f;
    } u;
    uint8_t kind;                /* DL_REC_PARAM / DL_REC_FRAME       */
} dl_record_t;

/* RAM param registry entry; db_id is per-open-file (re-interned lazily
 * after every open/rotate, 0 = not yet interned into this file) */
typedef struct
{
    bool used;
    char source[DL_SOURCE_MAX];
    char name[DL_NAME_MAX];
    int  db_id;
} dl_param_entry_t;

/* ---- storage engines (writer task ONLY) ----------------------------------

   Each stream binds one engine + one context at boot (`format` /
   `can_format` settings). All engines handle BOTH record kinds; a
   stream's files stay homogeneous because the streams are split by
   kind upstream. sqlite is the compatibility engine (~700 rows/s,
   BENCHMARKS.md); csv and binary are append logs. */
typedef struct
{
    const char *ext;                 /* ".db" / ".csv" / ".wdl" / …      */
    /* single_use: relative-timestamp / patched-header formats (mf4,
     * blf, asc) can't be appended across boots — open_current always
     * starts a fresh file for them */
    bool single_use;
    esp_err_t (*open)(void *ctx, const char *path, bool frames);
    void      (*close)(void *ctx);
    bool      (*is_open)(void *ctx);
    esp_err_t (*begin)(void *ctx);   /* transaction / no-op              */
    /* p = param registry entry for DL_REC_PARAM, NULL for frames */
    esp_err_t (*write)(void *ctx, const dl_record_t *rec,
                       dl_param_entry_t *p);
    esp_err_t (*commit)(void *ctx);  /* commit / flush                   */
    uint64_t  (*bytes)(void *ctx);   /* approx bytes in the current file */
    /* optional (ROBUSTNESS.md): "the last failure means the FILE is bad"
     * — the writer sets such a file aside instead of retrying it forever */
    bool      (*corrupt)(void *ctx);
    /* optional: never resume a file bigger than this (0 = no limit); the
     * .wdl torn-tail check is a full read, so huge CAN logs start fresh */
    uint64_t  resume_max;
} dl_engine_t;

extern const dl_engine_t dl_engine_sqlite;   /* data_logger_db.c     */
extern const dl_engine_t dl_engine_csv;      /* data_logger_append.c */
extern const dl_engine_t dl_engine_binary;   /* data_logger_append.c */
extern const dl_engine_t dl_engine_candump;  /* data_logger_append.c */
extern const dl_engine_t dl_engine_asc;      /* data_logger_append.c */
extern const dl_engine_t dl_engine_jsonl;    /* data_logger_append.c */
extern const dl_engine_t dl_engine_mf4;      /* data_logger_mf4.c    */
extern const dl_engine_t dl_engine_blf;      /* data_logger_blf.c    */

/* text/append format discriminator (dl_append_ctx_t.fmt) */
#define DL_AP_CSV     0
#define DL_AP_WDL     1
#define DL_AP_CANDUMP 2
#define DL_AP_ASC     3
#define DL_AP_JSONL   4

/* engine contexts — one static instance per (engine kind × stream) is
 * owned by data_logger.c; the structs live here so it can size them */
typedef struct
{
    void    *f;                  /* FILE*; void* keeps stdio out of
                                    the host-test build               */
    uint64_t bytes;
    char    *buf;                /* 4 KB DMA-capable stdio buffer —
                                    lazy INTERNAL heap, open→close     */
    uint16_t next_id;            /* .wdl dictionary ids               */
    uint8_t  fmt;                /* DL_AP_*                           */
    bool     frames;             /* header/row shape at open          */
    int64_t  start_ms;           /* asc: rows are relative to open    */
} dl_append_ctx_t;

typedef struct                   /* MDF 4.10 (data_logger_mf4.c)      */
{
    void    *f;
    uint64_t bytes;
    uint64_t records;
    int64_t  start_ms;
    uint32_t dt_len_off;         /* patch targets (commit)            */
    uint32_t cg_cycle_off;
    char    *buf;                /* lazy 4 KB DMA stdio buffer        */
} dl_mf4_ctx_t;

typedef struct                   /* Vector BLF (data_logger_blf.c)    */
{
    void    *f;
    uint64_t bytes;              /* file bytes incl. the 144 B header */
    uint64_t uncomp;             /* sum of container payload bytes    */
    uint32_t objects;            /* CAN_MESSAGE count                 */
    int64_t  start_ms;
    int64_t  last_ms;
    uint32_t cfill;              /* container accumulation fill       */
    char    *buf;                /* lazy 4 KB DMA stdio buffer        */
} dl_blf_ctx_t;

typedef struct
{
    void    *db;                 /* sqlite3*                          */
    void    *ins_rec;            /* prepared INSERT (records)         */
    void    *ins_frame;          /* prepared INSERT (frames)          */
    uint64_t rows;
    uint64_t frame_rows;
    bool     corrupt;            /* last error was SQLITE_CORRUPT/NOTADB */
} dl_sq_ctx_t;

/* rule-driven gate (logger.enable / logger.disable actions). While the
 * gate is off the writer parks but producers keep filling the rings, so
 * re-enabling flushes the newest records = pre-trigger context. */
void dl_runtime_gate(bool on);

/* frame-ring producers (data_logger_can.c drain task + CLI frametest) */
esp_err_t dl_frame_push(const dl_record_t *rec);

/* CAN stream lifecycle (data_logger_can.c) — settings snapshot applied
 * by data_logger.c's on_apply, task created by start when can_log */
typedef struct
{
    bool     log;
    uint32_t filter;
    uint32_t mask;
    bool     ext;
    bool     monitor_all;        /* can_filter == ""                  */
} dl_can_cfg_t;

void      dl_can_configure(const dl_can_cfg_t *cfg);
esp_err_t dl_can_start(void);    /* creates the drain task            */
uint32_t  dl_can_queued(void);   /* frames waiting in the sub queue   */
esp_err_t dl_can_test_push(int n); /* synthetic frames (CLI/bench)    */

/* ---- settings (data_logger_settings.c) ------------------------------------ */

/** Boot-applied configuration (from the settings descriptor). */
typedef struct
{
    bool         enabled;
    char         format[8];       /* param stream engine name          */
    char         can_format[8];   /* CAN stream engine name            */
    uint32_t     max_file_mb;     /* param stream rotation/retention   */
    uint32_t     max_files;
    uint32_t     can_max_file_mb; /* CAN stream rotation/retention     */
    uint32_t     can_max_files;
    uint32_t     batch_rows;
    uint32_t     flush_ms;
    uint32_t     ring_len;        /* frame ring live length (<= cap)   */
    uint8_t      autopid_log;     /* 0 off / 1 changed / 2 all         */
    dl_can_cfg_t can;
} dl_cfg_t;

/** Register the "data_logger" descriptor with settings_manager. */
esp_err_t dl_settings_register(void);

/** Boot-applied config; valid once dl_settings_is_configured(). */
const dl_cfg_t *dl_settings_config(void);
bool dl_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/** Apply-time engine/stream re-bind: data_logger.c owns the stream
 *  table and the rings; on_apply calls this after parsing. */
void dl_core_apply(const dl_cfg_t *cfg);

/* ---- recovery helpers (data_logger_recover.c — PURE, host-tested) ---------- */
size_t   dl_recover_text_keep(const char *buf, size_t len);
size_t   dl_recover_wdl_scan(const uint8_t *buf, size_t len, bool *bad);
bool     dl_recover_corrupt_name(const char *fname, char *out, size_t cap);
bool     dl_recover_is_corrupt_name(const char *fname, const char *prefix,
                                    int64_t *epoch_out);
bool     dl_recover_record_sane(const dl_record_t *r);
bool     dl_recover_ring_sane(uint32_t cap, uint32_t cap_limit, uint32_t head,
                              uint32_t tail, uint32_t *fill);
uint32_t dl_recover_crc32_update(uint32_t crc, const void *data, size_t len);
/* sqlite salvage (data_logger_db.c): copy what can still be read from @p src
 * into a fresh @p dst with the stream's schema; rows copied in *rows_out */
esp_err_t dl_sq_salvage(const char *src, const char *dst, bool frames,
                        uint32_t budget_ms, uint32_t *rows_out);

/* ---- data_logger_files.c — PURE (host-tested) ---------------------------- */

/* strict parse: <prefix><10 digits> + a known engine extension; any
 * known prefix (dl_/can_) is accepted */
bool dl_files_parse(const char *fname, int64_t *epoch_out);
/* writes "<prefix>%010lld<ext>" (zero-padded: lexical order = age) */
void dl_files_make(char *out, size_t len, const char *prefix,
                   int64_t epoch, const char *ext);

typedef struct
{
    char        oldest[48];
    char        newest[48];
    int         count;
    const char *prefix;          /* only names with this prefix fold  */
} dl_scan_t;

void dl_scan_init(dl_scan_t *s, const char *prefix);
/* fold one directory entry into the scan; ignores foreign names */
void dl_scan_add(dl_scan_t *s, const char *fname);

/* "7E8"/"0x7E8" → id; false on empty/garbage/overflow */
bool dl_parse_hex_u32(const char *s, uint32_t *out);

/* pure encoders (host-tested golden vectors) */
/* .wdl 0x03 frame: returns bytes written into buf (>= 22 cap) */
size_t dl_wdl_encode_frame(uint8_t *buf, int64_t ts_ms, uint32_t id,
                           uint8_t flags, const uint8_t *data,
                           uint8_t dlc);
/* csv frame row "ts,id_hex,ext,rtr,dlc,data_hex\n"; returns strlen */
int dl_csv_frame_row(char *out, size_t cap, int64_t ts_ms, uint32_t id,
                     uint8_t flags, const uint8_t *data, uint8_t dlc);
/* candump "(sec.usec) can0 ID#DATA\n" (can-utils / SavvyCAN) */
int dl_candump_row(char *out, size_t cap, int64_t ts_ms, uint32_t id,
                   uint8_t flags, const uint8_t *data, uint8_t dlc);
/* Vector .asc data row (seconds relative to start_ms, channel 1) */
int dl_asc_row(char *out, size_t cap, int64_t start_ms, int64_t ts_ms,
               uint32_t id, uint8_t flags, const uint8_t *data,
               uint8_t dlc);
/* JSON-lines rows */
int dl_jsonl_param_row(char *out, size_t cap, int64_t ts_ms,
                       const char *source, const char *name,
                       double value);
int dl_jsonl_frame_row(char *out, size_t cap, int64_t ts_ms,
                       uint32_t id, uint8_t flags, const uint8_t *data,
                       uint8_t dlc);

/* ---- MDF 4.10 builders (pure, little-endian) ------------------------------ */

#define DL_MF4_PRELUDE_MAX 2048
#define DL_MF4_REC_SIZE    22

/* ID..CN..DT-header prelude; returns total length and the two file
 * offsets commit() patches (DT block length u64, CG cycle_count u64) */
size_t dl_mf4_prelude(uint8_t *buf, size_t cap, int64_t start_ms,
                      uint32_t *dt_len_off, uint32_t *cg_cycle_off);
/* one 22-byte record: t f64 (s since start) | id u32 | flags u8 |
 * dlc u8 | data[8] */
size_t dl_mf4_record(uint8_t *buf, int64_t start_ms,
                     const dl_record_t *rec);

/* ---- Vector BLF builders (pure, little-endian) ----------------------------- */

#define DL_BLF_HDR_SIZE  144
#define DL_BLF_MSG_SIZE  48
#define DL_BLF_CONT_HDR  32

/* the 144-byte "LOGG" file header (re-written on every commit) */
size_t dl_blf_file_header(uint8_t *buf, int64_t start_ms, int64_t end_ms,
                          uint64_t file_size, uint64_t uncompressed,
                          uint32_t objects);
/* LOG_CONTAINER header for an UNCOMPRESSED payload of payload_len */
size_t dl_blf_container_header(uint8_t *buf, uint32_t payload_len);
/* one 48-byte CAN_MESSAGE object (ns timestamp relative to start_ms) */
size_t dl_blf_message(uint8_t *buf, int64_t start_ms,
                      const dl_record_t *rec);

/* ---- glue ----------------------------------------------------------------- */

void dl_events_register(void);                       /* declare sources  */
void dl_events_rotated(const char *file);            /* publish          */
void dl_events_error(const char *what, int code);    /* publish (rare)   */
