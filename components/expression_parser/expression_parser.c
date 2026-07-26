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
 * @file expression_parser.c
 * @brief The Automate expression evaluator — legacy grammar, bounds-checked.
 *
 * Adopted from the field-proven legacy evaluator
 * (autopid_legacy/expression_parser.c, shunting-yard over two fixed
 * stacks) with the 2026-07-06 changes: payload length checking, defined
 * unary minus, and hard errors where legacy read garbage (inverted
 * ranges, bit > 7, references past the payload). One code path serves
 * both eval and the data-less check() (dry-run mode) so the accepted
 * grammar can never drift between save-time validation and runtime.
 *
 * PURE: no IDF dependencies beyond esp_err.h — compiles unchanged on the
 * linux host target. No logging (hot path; errors are return values, and
 * check() carries the human-readable reason for the UI).
 */
#include "expression_parser.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EP_STACK_MAX 64  /* operands/operators; expressions are one-liners */

typedef struct
{
    /* inputs */
    const char    *expr;
    const uint8_t *data;
    size_t         data_len;
    double         v;
    bool           dry_run;       /* check(): fake byte reads, ignore /0 */

    /* state */
    double  operands[EP_STACK_MAX];
    char    operators[EP_STACK_MAX];
    int     n_operands;
    int     n_operators;
    bool    expect_operand;       /* a value must come next                  */
    bool    unary_ok;             /* '-' here is unary: start or after '('
                                     ONLY — legacy errored on "3*-2" and we
                                     keep that (write "3*(0-2)" instead)     */

    /* outputs */
    size_t     max_byte;          /* highest byte index referenced          */
    bool       any_byte;
    esp_err_t  err;
    const char *err_msg;
} ep_ctx_t;

static void fail(ep_ctx_t *ctx, esp_err_t err, const char *msg)
{
    if (ctx->err == ESP_OK)
    {
        ctx->err = err;
        ctx->err_msg = msg;
    }
}

static void push_operand(ep_ctx_t *ctx, double value)
{
    if (ctx->n_operands >= EP_STACK_MAX)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "expression too deep");
        return;
    }

    ctx->operands[ctx->n_operands++] = value;
    ctx->expect_operand = false;
    ctx->unary_ok = false;
}

/* Read byte @p index of the payload; dry runs record the index and fake 0. */
static double read_byte(ep_ctx_t *ctx, size_t index)
{
    if (!ctx->any_byte || index > ctx->max_byte)
    {
        ctx->max_byte = index;
        ctx->any_byte = true;
    }

    if (ctx->dry_run)
    {
        return 0.0;
    }

    if (index >= ctx->data_len)
    {
        fail(ctx, ESP_ERR_INVALID_SIZE, "byte reference beyond payload");
        return 0.0;
    }

    return (double)ctx->data[index];
}

static int precedence(char op)
{
    switch (op)
    {
        case '|':
        case '^': return 1;
        case '&': return 2;
        case '<':
        case '>': return 3; /* << and >> */
        case '+':
        case '-': return 4;
        case '*':
        case '/': return 5;
        default:  return 0; /* '(' */
    }
}

/* Pop one operator and apply it. Bitwise/shift truncate to 32-bit int —
   the legacy behavior user expressions were written against. */
static void apply_top(ep_ctx_t *ctx)
{
    char op = ctx->operators[--ctx->n_operators];

    if (ctx->n_operands < 2)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "operator is missing an operand");
        return;
    }

    double b = ctx->operands[--ctx->n_operands];
    double a = ctx->operands[--ctx->n_operands];
    double r = 0;

    switch (op)
    {
        case '+': r = a + b; break;
        case '-': r = a - b; break;
        case '*': r = a * b; break;
        case '/':
            if (b == 0)
            {
                if (!ctx->dry_run)
                {
                    fail(ctx, ESP_FAIL, "division by zero");
                    return;
                }

                b = 1; /* dry run: values are fake, /0 proves nothing */
            }

            r = a / b;
            break;
        case '&': r = (double)((int)a & (int)b); break;
        case '|': r = (double)((int)a | (int)b); break;
        case '^': r = (double)((int)a ^ (int)b); break;
        case '<': r = (double)((int)a << (int)b); break;
        case '>': r = (double)((int)a >> (int)b); break;
        default:
            fail(ctx, ESP_ERR_INVALID_ARG, "internal operator error");
            return;
    }

    ctx->operands[ctx->n_operands++] = r;
}

static void push_operator(ep_ctx_t *ctx, char op)
{
    while (ctx->err == ESP_OK && ctx->n_operators > 0 &&
           precedence(ctx->operators[ctx->n_operators - 1]) >=
               precedence(op))
    {
        apply_top(ctx);
    }

    if (ctx->n_operators >= EP_STACK_MAX)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "expression too deep");
        return;
    }

    ctx->operators[ctx->n_operators++] = op;
    ctx->expect_operand = true; /* unary_ok intentionally NOT set (legacy) */
}

/* Parse a 0+ digit run into @p out; false if no digit present. */
static bool parse_index(const char *s, size_t *i, size_t *out)
{
    if (!isdigit((unsigned char)s[*i]))
    {
        return false;
    }

    size_t value = 0;

    while (isdigit((unsigned char)s[*i]))
    {
        value = value * 10 + (size_t)(s[*i] - '0');
        (*i)++;
    }

    *out = value;
    return true;
}

/* [Bx:By] unsigned / [Sx:Sy] signed multi-byte, big-endian. Container
   width for signed spans follows legacy exactly: 1B->int8, 2B->int16,
   3..4B->int32, 5..8B->int64. */
static void parse_range(ep_ctx_t *ctx, size_t *i)
{
    const char *s = ctx->expr;
    size_t pos = *i + 1; /* past '[' */
    char kind = s[pos];
    size_t start = 0, end = 0;

    if (kind != 'B' && kind != 'S')
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "range must be [B..] or [S..]");
        return;
    }

    pos++;

    if (!parse_index(s, &pos, &start) || s[pos] != ':' ||
        s[pos + 1] != kind)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "bad range syntax");
        return;
    }

    pos += 2;

    if (!parse_index(s, &pos, &end) || s[pos] != ']')
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "bad range syntax");
        return;
    }

    pos++; /* past ']' */

    if (end < start)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "inverted range");
        return;
    }

    if (end - start > 7)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "range wider than 8 bytes");
        return;
    }

    uint64_t value = 0;

    for (size_t j = start; j <= end && ctx->err == ESP_OK; j++)
    {
        value = (value << 8) | (uint64_t)(uint8_t)read_byte(ctx, j);
    }

    if (ctx->err != ESP_OK)
    {
        return;
    }

    double out;

    if (kind == 'B')
    {
        out = (double)value;
    }
    else
    {
        size_t span = end - start + 1;

        if (span == 1)
        {
            out = (double)(int8_t)value;
        }
        else if (span == 2)
        {
            out = (double)(int16_t)value;
        }
        else if (span <= 4)
        {
            out = (double)(int32_t)value;
        }
        else
        {
            out = (double)(int64_t)value;
        }
    }

    *i = pos;
    push_operand(ctx, out);
}

/* B<n>, B<n>:<bit>, S<n> */
static void parse_byte_ref(ep_ctx_t *ctx, size_t *i)
{
    const char *s = ctx->expr;
    char kind = s[*i];
    size_t pos = *i + 1;
    size_t index = 0;

    if (!parse_index(s, &pos, &index))
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "byte reference needs an index");
        return;
    }

    double value = read_byte(ctx, index);

    if (kind == 'S')
    {
        value = (double)(int8_t)(uint8_t)value;
    }
    else if (s[pos] == ':')
    {
        pos++;

        if (!isdigit((unsigned char)s[pos]) || s[pos] > '7')
        {
            fail(ctx, ESP_ERR_INVALID_ARG, "bit index must be 0..7");
            return;
        }

        value = (double)(((uint8_t)value >> (s[pos] - '0')) & 1u);
        pos++;
    }

    if (ctx->err != ESP_OK)
    {
        return;
    }

    *i = pos;
    push_operand(ctx, value);
}

static void evaluate(ep_ctx_t *ctx, double *result)
{
    const char *s = ctx->expr;
    size_t i = 0;

    ctx->expect_operand = true;
    ctx->unary_ok = true;

    while (s[i] != '\0' && ctx->err == ESP_OK)
    {
        char c = s[i];

        if (isspace((unsigned char)c))
        {
            i++;
        }
        else if (isdigit((unsigned char)c) || c == '.')
        {
            char *end = NULL;
            double value = strtod(&s[i], &end);

            /* advance over digits/'.' only — no exponent notation, same
               as legacy (an 'e' after a number is a syntax error) */
            while (isdigit((unsigned char)s[i]) || s[i] == '.')
            {
                i++;
            }

            (void)end;
            push_operand(ctx, value);
        }
        else if (c == 'V')
        {
            push_operand(ctx, ctx->v);
            i++;
        }
        else if (c == '[')
        {
            parse_range(ctx, &i);
        }
        else if (c == 'B' || c == 'S')
        {
            parse_byte_ref(ctx, &i);
        }
        else if (c == '(')
        {
            if (ctx->n_operators >= EP_STACK_MAX)
            {
                fail(ctx, ESP_ERR_INVALID_ARG, "expression too deep");
                break;
            }

            ctx->operators[ctx->n_operators++] = '(';
            ctx->expect_operand = true;
            ctx->unary_ok = true;
            i++;
        }
        else if (c == ')')
        {
            while (ctx->err == ESP_OK && ctx->n_operators > 0 &&
                   ctx->operators[ctx->n_operators - 1] != '(')
            {
                apply_top(ctx);
            }

            if (ctx->err == ESP_OK &&
                (ctx->n_operators == 0 ||
                 ctx->operators[ctx->n_operators - 1] != '('))
            {
                fail(ctx, ESP_ERR_INVALID_ARG, "mismatched parentheses");
            }

            if (ctx->err == ESP_OK)
            {
                ctx->n_operators--; /* discard '(' */
            }

            i++;
        }
        else if (c == '-' && ctx->unary_ok)
        {
            /* unary minus at expression start / after '(' — legacy
               accepted exactly these by accident (empty-stack pop gave
               0); defined here as 0 - operand */
            push_operand(ctx, 0.0);
            ctx->expect_operand = true; /* still awaiting the operand */
            push_operator(ctx, '-');
            i++;
        }
        else if (c == '+' || c == '-' || c == '*' || c == '/' ||
                 c == '&' || c == '|' || c == '^' ||
                 (c == '<' && s[i + 1] == '<') ||
                 (c == '>' && s[i + 1] == '>'))
        {
            if (ctx->expect_operand)
            {
                fail(ctx, ESP_ERR_INVALID_ARG,
                     "operator where a value was expected");
                break;
            }

            if (c == '<' || c == '>')
            {
                i++; /* << and >> are one operator */
            }

            push_operator(ctx, c);
            i++;
        }
        else
        {
            fail(ctx, ESP_ERR_INVALID_ARG, "invalid character");
        }
    }

    while (ctx->err == ESP_OK && ctx->n_operators > 0)
    {
        if (ctx->operators[ctx->n_operators - 1] == '(')
        {
            fail(ctx, ESP_ERR_INVALID_ARG, "mismatched parentheses");
            break;
        }

        apply_top(ctx);
    }

    if (ctx->err == ESP_OK && ctx->n_operands != 1)
    {
        fail(ctx, ESP_ERR_INVALID_ARG, "expression yields no single value");
    }

    if (ctx->err == ESP_OK && result != NULL)
    {
        *result = ctx->operands[0];
    }
}

esp_err_t expression_parser_eval(const char *expr, const uint8_t *data,
                                 size_t data_len, double v, double *result)
{
    if (expr == NULL || result == NULL || (data == NULL && data_len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ep_ctx_t ctx =
    {
        .expr     = expr,
        .data     = data,
        .data_len = data_len,
        .v        = v,
        .err      = ESP_OK,
    };

    evaluate(&ctx, result);
    return ctx.err;
}

esp_err_t expression_parser_check(const char *expr, size_t *max_byte_out,
                                  char *err, size_t err_len)
{
    if (max_byte_out != NULL)
    {
        *max_byte_out = SIZE_MAX;
    }

    if (expr == NULL)
    {
        if (err != NULL && err_len > 0)
        {
            snprintf(err, err_len, "no expression");
        }

        return ESP_ERR_INVALID_ARG;
    }

    ep_ctx_t ctx =
    {
        .expr    = expr,
        .v       = 0,
        .dry_run = true,
        .err     = ESP_OK,
    };
    double ignored;

    evaluate(&ctx, &ignored);

    if (ctx.err != ESP_OK)
    {
        if (err != NULL && err_len > 0)
        {
            snprintf(err, err_len, "%s",
                     (ctx.err_msg != NULL) ? ctx.err_msg : "invalid");
        }

        return ESP_ERR_INVALID_ARG;
    }

    if (max_byte_out != NULL && ctx.any_byte)
    {
        *max_byte_out = ctx.max_byte;
    }

    return ESP_OK;
}
