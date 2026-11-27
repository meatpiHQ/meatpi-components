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
 * @file script_engine_doc_flash.c
 * @brief The reference entries of the flash_* bindings (pure: the host
 *        suite checks them with the rest of the table). Split from
 *        script_engine_doc.c for the 700-line rule.
 */
#include "script_engine_doc.h"

static const se_bind_doc_t FLASH_DOCS[] =
{
    { "flash_isotp", "flash_isotp(tx_id, rx_id[, opts])", "flash", "session handle",
      "Open an ISO-TP session on any IDs. opts: ext, ae_tx/ae_rx (extended "
      "address bytes, -1 for none), pad (a byte; -1 or false for none), bs, "
      "stmin, gap_us (least time between our consecutive frames, up to "
      "127000). Four at most; can_stop() closes them. Needs Allow ECU "
      "flashing.",
      "var ldr = flash_isotp(0x001, 0x002, {'pad': 0xAA, 'gap_us': 300})" },
    { "flash_send", "flash_send(h, bytes[, timeout_ms])", "flash", "true",
      "Send one PDU of up to 4095 bytes; raises when the ECU's flow control "
      "never comes.",
      "flash_send(ldr, bytes('76'))" },
    { "flash_recv", "flash_recv(h[, timeout_ms])", "flash", "bytes, or nil",
      "Receive the next PDU on a session (up to 4095 bytes). The timeout runs "
      "until the first frame, then between frames; 0 looks once.",
      "var ack = flash_recv(ldr, 1000)" },
    { "flash_request", "flash_request(h, bytes[, timeout_ms[, opts]])", "flash",
      "the reply bytes, or nil",
      "Send and wait for the reply, waiting through 7F xx 78. A negative "
      "reply raises 'nrc' unless opts has raw: true. opts: pending_ms, "
      "retries, raw.",
      "var seed = flash_request(ecu, bytes('2701'), 1500)" },
    { "flash_close", "flash_close(h)", "flash", "nil",
      "Close a session (every session also closes when the run ends).",
      "flash_close(ldr)" },
    { "flash_can_send", "flash_can_send(id, bytes[, ext])", "flash", "true",
      "Send one raw frame of up to 8 bytes. Frames queued for flash_can_recv "
      "before it are dropped, so a recv reads its answer. Needs Allow ECU "
      "flashing.",
      "flash_can_send(0x750, bytes('5F022751'))" },
    { "flash_can_recv", "flash_can_recv(timeout_ms[, ids])", "flash",
      "[id, bytes], or nil",
      "The next frame on the bus, or the next one with that id (an int) or "
      "an id in the list (1 to 16 ints). 0 ms looks once.",
      "var f = flash_can_recv(600, [0x758])" },
    { "flash_can_quiet", "flash_can_quiet(window_ms, timeout_ms[, ids])", "flash",
      "bool",
      "Wait until no frame (or none with these ids) was seen for window_ms; "
      "false when timeout_ms ran out first.",
      "flash_can_quiet(1200, 5000, [0x1C4])" },
    { "flash_commit", "flash_commit([label])", "flash", "true",
      "The point of no return, just before the first erase; async runs only. "
      "Records it in NVS, then locks the device: only flash_* bindings reach "
      "the bus, the OBD chip sends nothing, max_runtime_ms stops counting "
      "and Stop becomes flash_abort_requested(). Refused during a firmware "
      "update or with an obd_claim() open. A run that ends without an error "
      "and was not asked to abort opens the lock; any other end keeps it.",
      "flash_commit('erase')" },
    { "flash_abort_requested", "flash_abort_requested()", "flash", "bool",
      "True once Stop was pressed after the commit: finish the current step "
      "and leave the ECU recoverable. The run then ends failed and the lock "
      "stays, however it returns.",
      "if flash_abort_requested() return end" },
    { "flash_state", "flash_state(state)", "flash", "nil",
      "Tell the status what the ECU holds now: 'untouched', 'loader' or "
      "'unknown'.",
      "flash_state('loader')" },
    { "flash_phase", "flash_phase(name)", "flash", "nil",
      "Name the step the run is in, for the status.",
      "flash_phase('write')" },
    { "flash_progress", "flash_progress(done, total)", "flash", "nil",
      "Report progress, in whatever unit the script likes.",
      "flash_progress(off, size)" },
    { "flash_log", "flash_log(msg)", "flash", "nil",
      "Add a numbered line to the status log (GET /api/scripts/status?since=n).",
      "flash_log('erase acknowledged')" },
    { "flash_ask", "flash_ask(text[, choices])", "flash", "the answer string",
      "Show the operator a prompt and wait for POST /api/scripts/answer "
      "(one answer per prompt). Async runs only.",
      "flash_ask('Turn the ignition off, then on', 'done')" },
    { "flash_param", "flash_param(name)", "flash", "value, or nil",
      "A parameter the run was started with (\"params\" of POST /api/scripts/run).",
      "var cal = flash_param('calibration')" },
    { "flash_micros", "flash_micros()", "flash", "int",
      "Microseconds since boot.",
      "var t0 = flash_micros()" },
    { "flash_file_size", "flash_file_size(path)", "flash", "byte count, or nil",
      "Size of a file on the SD card ('/sd/...' only).",
      "var n = flash_file_size('/sd/flash/records.bin')" },
    { "flash_file_read", "flash_file_read(path, offset, len)", "flash",
      "bytes, or nil",
      "Read up to 4096 bytes of an SD-card file, on an internal stack.",
      "var piece = flash_file_read('/sd/flash/records.bin', off, 1024)" },
    { "flash_file_write", "flash_file_write(path, bytes[, append])", "flash", "true",
      "Write or append bytes to an SD-card file (a backup, a readback).",
      "flash_file_write('/sd/flash/backup.bin', data, true)" },
    { "flash_file_sha256", "flash_file_sha256(path)", "flash", "hex string, or nil",
      "SHA-256 of a whole SD-card file: check a staged image before the commit.",
      "if flash_file_sha256(p) != want raise 'flash_error', 'bad image' end" },
    { "flash_crc", "flash_crc(bytes, width, poly, init, refin, refout, xorout)",
      "flash", "int",
      "Any CRC of up to 32 bits (Rocksoft model). With only bytes: CRC-32.",
      "var c = flash_crc(block, 16, 0x1021, 0xFFFF, false, false, 0)" },
    { "flash_adler32", "flash_adler32(bytes[, adler])", "flash", "int",
      "Adler-32, continuing from adler (1 to start).",
      "var a = flash_adler32(block)" },
    { "flash_sum", "flash_sum(bytes, width[, big_endian])", "flash", "int",
      "Sum of 1-, 2- or 4-byte words, modulo 2^32.",
      "var s = flash_sum(block, 4)" },
    { "flash_sha256", "flash_sha256(bytes)", "flash", "hex string",
      "SHA-256 of a bytes value, as 64 hex characters.",
      "var h = flash_sha256(bytes('00'))" },
};

size_t se_flash_doc_count(void)
{
    return sizeof(FLASH_DOCS) / sizeof(FLASH_DOCS[0]);
}

const se_bind_doc_t *se_flash_doc(size_t i)
{
    return (i < se_flash_doc_count()) ? &FLASH_DOCS[i] : NULL;
}
