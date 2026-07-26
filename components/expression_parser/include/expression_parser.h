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
 * @file expression_parser.h
 * @brief WiCAN parameter-expression evaluator (the Automate grammar).
 *
 * Pure utility component (no IDF dependencies beyond esp_err) evaluating
 * the expression format documented at
 * meatpihq.github.io/wican-fw/config/automate/usage — the grammar existing
 * user expressions were written against, kept compatible:
 *
 *   B<n>          unsigned byte n of the payload
 *   B<n>:<bit>    single bit (0..7) of byte n
 *   S<n>          signed byte n
 *   [B<x>:B<y>]   unsigned big-endian multi-byte (span <= 8 bytes)
 *   [S<x>:S<y>]   signed big-endian multi-byte — container by span, exactly
 *                 the legacy semantics: 1 byte -> int8, 2 -> int16,
 *                 3..4 -> int32, 5..8 -> int64 (a 3-byte span therefore has
 *                 no sign bit in range, as legacy behaved)
 *   V             battery voltage (volts)
 *   numbers       decimal literals (digits and '.')
 *   operators     + - * / << >> & | ^ and parentheses, C-like precedence
 *                 (* / over + - over << >> over & over ^ |); bitwise and
 *                 shift operands truncate to 32-bit int as legacy did
 *   unary minus   supported (legacy accepted it by accident; now defined)
 *
 * Differences from the legacy evaluator (deliberate, 2026-07-06 meatpi):
 *  - The payload LENGTH is a parameter: any B/S/[..] reference beyond
 *    data_len-1 returns ESP_ERR_INVALID_SIZE instead of reading past the
 *    buffer.
 *  - Inverted ranges ([B3:B0]) and bit indexes > 7 are errors instead of
 *    silent garbage.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Evaluate @p expr against a response payload.
 *
 * @param expr      NUL-terminated expression (see grammar above).
 * @param data      Payload bytes the B/S references index into. May be NULL
 *                  only when @p data_len is 0.
 * @param data_len  Number of valid bytes at @p data.
 * @param v         The V variable (battery voltage, volts).
 * @param[out] result  The value on success.
 *
 * @return ESP_OK;
 *         ESP_ERR_INVALID_ARG  malformed expression (syntax, span > 8,
 *                              inverted range, bit > 7, arity errors) or
 *                              NULL expr/result;
 *         ESP_ERR_INVALID_SIZE a byte reference beyond @p data_len;
 *         ESP_FAIL             runtime math failure (division by zero).
 */
esp_err_t expression_parser_eval(const char *expr, const uint8_t *data,
                                 size_t data_len, double v, double *result);

/**
 * @brief Validate @p expr WITHOUT data — the save-time / UI check.
 *
 * Runs the same tokenizer/evaluator in dry-run mode: byte references
 * evaluate as 0 and only their indexes are recorded; division by zero is
 * ignored (values are fake). Reports the highest byte index referenced so
 * a PID's expected response length can be sanity-checked at config save.
 *
 * @param expr          NUL-terminated expression.
 * @param[out] max_byte_out  Highest 0-based byte index referenced; SIZE_MAX
 *                           when the expression references no bytes. May be
 *                           NULL.
 * @param[out] err      Human-readable reason on failure (for the UI/API).
 *                      May be NULL.
 * @param err_len       Size of @p err.
 *
 * @return ESP_OK on a valid expression, ESP_ERR_INVALID_ARG otherwise.
 */
esp_err_t expression_parser_check(const char *expr, size_t *max_byte_out,
                                  char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
