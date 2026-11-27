/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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
 * @file script_engine_flash_core.h
 * @brief The pure parts of ECU flashing from scripts, host-tested: the
 *        device lock's states and rules, and the checksum helpers.
 *
 * The lock. A script marks its point of no return with flash_commit():
 * from then until the run ends well, or an operator releases it, the ECU
 * may be half-written and nothing else on the WiCAN may disturb the job.
 *
 *   IDLE ──flash_commit()──▶ COMMITTED ──run ends ok──▶ IDLE
 *                               │
 *               run ends with an error, or after an abort request
 *                               ▼
 *                            FAILED ──release (acknowledged)──▶ IDLE
 *   boot with the NVS record ⇒ RECOVERY (like FAILED)
 *
 * While the lock holds, only the script runner may transmit, and HTTP
 * requests that change anything are refused (423) except the script
 * engine's own run control. A run in FAILED or RECOVERY may commit again
 * (a recovery script).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SE_LOCK_IDLE = 0,
    SE_LOCK_COMMITTED,  /**< a run passed its point of no return          */
    SE_LOCK_FAILED,     /**< that run ended with an error                 */
    SE_LOCK_RECOVERY,   /**< the WiCAN restarted while committed          */
} se_lock_state_t;

const char *se_lock_state_str(se_lock_state_t s);

/** The lock holds: committed, failed or recovery. */
bool se_lock_held(se_lock_state_t s);

se_lock_state_t se_lock_on_commit(se_lock_state_t s);
/** A committed run ends: IDLE only when it raised nothing AND was not
 *  asked to abort (an aborted flash leaves a half-written ECU: FAILED). */
se_lock_state_t se_lock_on_run_end(se_lock_state_t s, bool ok, bool aborted);
/** Only FAILED and RECOVERY can be released; COMMITTED never. */
se_lock_state_t se_lock_on_release(se_lock_state_t s);
se_lock_state_t se_lock_on_boot(bool committed_record);

/** May this frame go out? @p from_runner: sent by the script runner. */
bool se_lock_tx_allowed(se_lock_state_t s, bool from_runner);

/**
 * May this HTTP request run? @p read_only: GET, HEAD or OPTIONS.
 * @p uri may carry a query. @p run_active: a script is running now.
 */
bool se_lock_http_allowed(se_lock_state_t s, bool read_only,
                          const char *uri, bool run_active);

/* ---- checksum helpers (the flash_* bindings) ------------------------------ */

typedef struct
{
    uint8_t  width;   /**< 1..32                                           */
    uint32_t poly;
    uint32_t init;
    bool     refin;
    bool     refout;
    uint32_t xorout;
} se_crc_spec_t;

/** A parametric CRC (the Rocksoft model), bit by bit: scripts check
 *  blocks of a few KiB, so speed isn't the point. false: bad width. */
bool se_crc(const se_crc_spec_t *spec, const uint8_t *d, size_t n,
            uint32_t *out);

/** Adler-32, continuing from @p adler (1 to start). */
uint32_t se_adler32(uint32_t adler, const uint8_t *d, size_t n);

/** The sum of @p n bytes as words of @p width bytes (1, 2 or 4),
 *  little- or big-endian, modulo 2^32. A trailing partial word is summed
 *  as if zero-padded. */
uint32_t se_word_sum(const uint8_t *d, size_t n, uint8_t width,
                     bool big_endian);

#ifdef __cplusplus
}
#endif
