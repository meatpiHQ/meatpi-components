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
 * @file can_core_filter.h
 * @brief CAN frame filter and mask utilities for the ELM327 emulator.
 *
 * Provides helper functions for checking whether a given frame ID passes a
 * filter / mask combination, and for formatting filter / mask values as
 * hex strings for the CS and PPS responses.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Test whether a CAN frame ID passes a filter / mask combination.
 *
 * The ELM327 uses the convention: a bit set to 1 in the mask means that
 * bit in the ID *must* equal the corresponding bit in the filter.
 *
 *   pass = (id & mask) == (filter & mask)
 *
 * @param id      Frame ID to test.
 * @param filter  Filter pattern.
 * @param mask    Mask (1 = bit is significant).
 * @return true if the frame passes, false if it should be dropped.
 */
bool can_core_filter_match(uint32_t id, uint32_t filter, uint32_t mask);

/**
 * @brief Format an 11-bit CAN ID as a 3-character uppercase hex string.
 *
 * @param id   11-bit CAN ID (bits 10–0 used).
 * @param buf  Output buffer (must be at least 4 bytes).
 */
void can_core_format_id_11(uint32_t id, char *buf);

/**
 * @brief Format a 29-bit CAN ID as an 8-character uppercase hex string.
 *
 * @param id   29-bit CAN ID (bits 28–0 used).
 * @param buf  Output buffer (must be at least 9 bytes).
 */
void can_core_format_id_29(uint32_t id, char *buf);

/**
 * @brief Parse a hex string to a CAN ID.
 *
 * Accepts 3 hex digits (11-bit) or 8 hex digits (29-bit).
 * All other lengths return false.
 *
 * @param str      Input string (NUL-terminated, upper or lower case).
 * @param out_id   Receives the parsed ID value.
 * @param out_ext  Receives true for 29-bit, false for 11-bit.
 * @return true on success, false if the string is invalid.
 */
bool can_core_parse_id(const char *str,
                          uint32_t *out_id,
                          bool *out_ext);

/**
 * @brief Parse a hex byte string (1–2 hex digits).
 *
 * @param str      Input string.
 * @param out_byte Receives parsed byte value.
 * @return true on success.
 */
bool can_core_parse_byte(const char *str, uint8_t *out_byte);

/**
 * @brief Parse a sequence of hex byte pairs into a buffer.
 *
 * The input string may contain optional space separators.
 *
 * @param str      Input string (e.g. "11 22 33").
 * @param out_buf  Output buffer.
 * @param max_len  Maximum number of bytes to parse.
 * @param out_len  Receives the actual number of bytes parsed.
 * @return true if at least one byte was parsed and no parse error occurred.
 */
bool can_core_parse_bytes(const char *str,
                             uint8_t *out_buf,
                             size_t max_len,
                             size_t *out_len);

/**
 * @brief Format an array of bytes as a hex string with optional spaces.
 *
 * @param data    Input byte array.
 * @param len     Number of bytes.
 * @param buf     Output buffer.
 * @param buf_sz  Size of output buffer.
 * @param spaces  If true, separate each byte with a space.
 */
void can_core_format_bytes(const uint8_t *data,
                              size_t len,
                              char *buf,
                              size_t buf_sz,
                              bool spaces);

#ifdef __cplusplus
}
#endif
