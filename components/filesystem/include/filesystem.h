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
 * @file filesystem.h
 * @brief WiCAN general-purpose file API (core component).
 *
 * Owns the internal-flash data filesystem (LittleFS on the `storage`
 * partition) and presents one path-based interface. Callers use logical
 * paths; the component routes them to the right backend:
 *
 *   /data/...   internal flash (LittleFS): firmware-managed data
 *   /sd/...     SD card via external_storage: bulk/web/log data
 *
 * Pinned behaviors (ARCHITECTURE.md §5, REVIEW.md §6.1):
 *   - Path namespace: a path is addressed by its logical prefix above. There
 *     is no transparent fallback: a caller that wants SD data asks for /sd.
 *   - SD absent / backend unavailable: operations return
 *     ESP_ERR_INVALID_STATE. (v1 ships the internal backend only; /sd is
 *     reserved and unavailable until external_storage exists.)
 *   - filesystem_write() is ALWAYS atomic (temp file + fsync + rename) and
 *     auto-creates parent directories. Callers never reimplement safe writes.
 *   - filesystem_list() exists so UIs can enumerate candidates for the
 *     settings schema `format:"file"` keyword.
 *
 * Concurrency: all operations are internally serialized with one mutex.
 * Handles from filesystem_open() are plain stdio streams owned by the
 * caller and are NOT covered by that serialization.
 *
 * Memory (Coding Standard §2, ARCHITECTURE §12): read/write data is bounced
 * through an internal-RAM scratch buffer (cache-off during flash ops), so
 * caller buffers may live in PSRAM. Every /data operation (reads included:
 * littlefs reads go through esp_partition_read, which disables the flash
 * cache too, bench-proven 2026-07-07 via the event dispatcher) needs a task
 * stack in internal RAM. Since 2026-10-13 the component takes care of that
 * itself: an operation called from a task whose stack is in PSRAM runs on
 * an internal-stack worker (the shared flash_jobs worker since 2026-10-31),
 * synchronously, one at a time (the caller waits; a filesystem_list()
 * callback then runs on that worker's 6 KB stack, so keep callbacks small).
 * A stream from filesystem_open() is the caller's: use filesystem_fread(),
 * filesystem_fstat() and filesystem_fclose() on a /data stream from a
 * PSRAM-stack task (they marshal the call), plain stdio only from an
 * internal-stack task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Per-entry callback for filesystem_list(). @p name is the entry name only
 * (no directory part), valid for the duration of the call. @p size is 0 for
 * directories. Return ESP_OK to continue enumeration; any other value stops
 * it and is returned from filesystem_list().
 */
typedef esp_err_t (*filesystem_list_cb_t)(const char *name, bool is_dir,
                                          size_t size, void *ctx);

/**
 * Mount the internal data filesystem (LittleFS, partition label
 * CONFIG_FILESYSTEM_PARTITION_LABEL, formatted on first use).
 * Call once from main before any dependent component's init.
 */
esp_err_t filesystem_init(void);

/** Begin operation. Requires a successful init. */
esp_err_t filesystem_start(void);

/** Stop and unmount. After stop, init() may be called again (test flows). */
esp_err_t filesystem_stop(void);

/**
 * FACTORY RESET (2026-10-26): unmount the internal data filesystem and
 * erase its partition raw, so nothing of /data stays on the flash (the
 * vehicles, databases, scripts, dashboard, certificate sets, the backup
 * copy, the web UI cache). The SD card is never touched. Every /data and
 * /sd call answers ESP_ERR_INVALID_STATE afterwards: the caller MUST
 * restart (restart_tracker, reason FACTORY_RESET); the next boot finds the
 * partition blank and formats it quietly (the first-boot path). Works
 * before init too (safe mode: nothing mounted, the erase alone). Flash:
 * the erase runs on an internal stack whatever the caller's.
 */
esp_err_t filesystem_wipe(void);

/**
 * Atomically replace @p path with @p data (temp file + fsync + rename;
 * a power cut leaves either the old file or the new one, never a torn one).
 * Missing parent directories are created. @p len == 0 writes an empty file.
 */
esp_err_t filesystem_write(const char *path, const void *data, size_t len);

/* ---- streaming writes (big files: downloads, future streamed uploads) ----
 * Same atomicity as filesystem_write, chunk by chunk: open targets a
 * TEMP sibling, commit fsync+renames it over @p path, abort deletes it,
 * an interrupted stream never leaves a torn file. ONE stream at a time
 * (ESP_ERR_INVALID_STATE when busy). write_chunk serializes with other
 * fs ops per chunk, so a long stream doesn't starve them. The §2
 * corollary applies to the CHUNK CALLER for /data paths (flash writes
 * need an internal stack); /sd chunk writes don't touch the flash cache. */

typedef struct filesystem_wstream filesystem_wstream_t;

esp_err_t filesystem_write_open(const char *path,
                                filesystem_wstream_t **out);
esp_err_t filesystem_write_chunk(filesystem_wstream_t *ws,
                                 const void *data, size_t len);
esp_err_t filesystem_write_commit(filesystem_wstream_t *ws);
void filesystem_write_abort(filesystem_wstream_t *ws);

/* ---- the ASYNC pipe on top of a wstream (big transfers) ------------------
 * PRODUCER/WRITER overlap: async_chunk copies the producer's bytes into
 * one of two internal 32 KB PSRAM slots and returns, the filesystem's
 * own INTERNAL-stack writer task drains slots to the media concurrently,
 * so network receive and media writes overlap (the slower side sets the
 * pace). The producer's task never performs flash writes (its stack may
 * be PSRAM). async_end drains, then commits (fsync+rename) or aborts:
 * the same never-a-torn-file guarantee. */

esp_err_t filesystem_write_async_begin(filesystem_wstream_t *ws);
esp_err_t filesystem_write_async_chunk(filesystem_wstream_t *ws,
                                       const void *data, size_t len);
esp_err_t filesystem_write_async_end(filesystem_wstream_t *ws,
                                     bool commit);

/**
 * Read the whole file into @p buf. @p out_len (nullable) receives the file
 * size. Returns ESP_ERR_INVALID_SIZE if the file is larger than @p buf_len
 * (out_len still reports the required size); ESP_ERR_NOT_FOUND if missing.
 */
esp_err_t filesystem_read(const char *path, void *buf, size_t buf_len,
                          size_t *out_len);

/** File size in bytes. ESP_ERR_NOT_FOUND if missing. */
esp_err_t filesystem_size(const char *path, size_t *out_size);

/** True if @p path exists (file or directory). False on any error. */
bool filesystem_exists(const char *path);

/** Delete a file (or an empty directory). ESP_ERR_NOT_FOUND if missing. */
esp_err_t filesystem_delete(const char *path);

/** Create @p dir_path and any missing parents. Existing dir is ESP_OK. */
esp_err_t filesystem_mkdirs(const char *dir_path);

/** Enumerate a directory's entries. See filesystem_list_cb_t. */
esp_err_t filesystem_list(const char *dir_path, filesystem_list_cb_t cb,
                          void *ctx);

/**
 * Paths the generic HTTP file surface (/api/fs, the Files page) must not
 * reach: the certificate sets and the device's own backup copies, which
 * hold key material (config_backup, 2026-10-11). True for the folder
 * itself and anything under it; /sd compares without case (FAT). Pure:
 * the component's own API (and every other component) is unaffected.
 */
bool filesystem_api_hidden(const char *path);

/**
 * Validated streaming open (for chunked serving: http_server_manager).
 * Returns NULL on invalid path, unavailable backend, or fopen failure.
 * NOTE: streaming writes bypass the atomic-replace guarantee; prefer
 * filesystem_write() for anything that must survive a power cut.
 * The open of a /data stream runs on the worker for a PSRAM-stack caller
 * (the directory walk reads the flash).
 */
FILE *filesystem_open(const char *path, const char *mode);

/**
 * The stream helpers for a PSRAM-stack caller (2026-10-31): fread, fstat
 * and fclose on a filesystem_open() handle, run on the worker when the
 * calling task's stack is in PSRAM, in line otherwise. They do not know
 * the handle's backend, so a call from a PSRAM stack is marshalled for
 * /sd streams too (harmless). filesystem_fread() returns the bytes read
 * (0 at the end or on an error, as fread does).
 */
size_t    filesystem_fread(void *buf, size_t len, FILE *f);
esp_err_t filesystem_fstat(FILE *f, struct stat *st);
esp_err_t filesystem_fclose(FILE *f);

/**
 * Capacity of the backend owning @p prefix ("/data" or "/sd"), in bytes.
 * Either out pointer may be NULL. 64-bit on purpose: a card is bigger than
 * 4 GiB, and the size_t these were until 2026-10-11 wrapped a 32 GB card to
 * 976 MB in the File Manager (issue #982).
 */
esp_err_t filesystem_info(const char *prefix, uint64_t *out_total,
                          uint64_t *out_used);

/**
 * Enable/disable the /sd backend. Called by the COMPOSITION ROOT from
 * external_storage's mount-event callback (main knows both components;
 * neither depends on the other). external_storage mounts the card's FAT
 * volume at the "/sd" VFS root, so logical paths map 1:1. While disabled,
 * /sd operations return ESP_ERR_INVALID_STATE.
 */
esp_err_t filesystem_sd_set_mounted(bool mounted);

/** Register the `fs` CLI command with cmdline_manager. Called
 *  INTERNALLY on the settings boot apply when the `cli` setting is true
 *  (default): main no longer wires it. */
esp_err_t filesystem_register_cli(void);

/** Register the settings descriptor ({cli}). Init runs before
 *  settings_manager_init, so the composition root calls this separately
 *  (the log_manager_register_settings pattern). */
esp_err_t filesystem_register_settings(void);

#ifdef __cplusplus
}
#endif
