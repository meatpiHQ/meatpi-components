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
 * @file autopid_dbc_codec.c
 * @brief PURE DBC codec (TASK_dbc.md §2-3, host-tested): parse the BO_/
 *        SG_/SIG_VALTYPE_ subset, and COMPILE a signal into an
 *        expression_parser expression over the filter payload (B0 =
 *        first frame data byte — the same frame of reference DBC uses).
 *
 * Compilation rules (why multiply-add, not shift-or): the expression
 * grammar truncates bitwise/shift operands to 32-bit int, so int ops
 * are only ever applied WITHIN one byte ((B2>>4)&15); multi-byte
 * composition and sign extension use * + - on exact power-of-two
 * decimal literals (doubles — exact to 2^53). Motorola byte-aligned
 * signals use the compact [Bx:By]/[Sx:Sy] spans (spans are big-endian
 * = Motorola order; signed spans only for 1/2/4/8-byte containers —
 * the 3-byte-span no-sign-bit legacy quirk).
 */
#include "autopid_private.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- literal formatting (grammar has digits + '.', NO exponent) --------------- */

static void fmt_num(double v, char *out, size_t cap)
{
    snprintf(out, cap, "%.10g", v);

    if (strchr(out, 'e') != NULL || strchr(out, 'E') != NULL)
    {
        snprintf(out, cap, "%.15f", v);

        char *dot = strchr(out, '.');

        if (dot != NULL)
        {
            char *end = out + strlen(out) - 1;

            while (end > dot + 1 && *end == '0')
            {
                *end-- = '\0';
            }
        }
    }
}

/* exact integer (powers of two up to 2^56) */
static void fmt_int(double v, char *out, size_t cap)
{
    snprintf(out, cap, "%.0f", v);
}

/* ---- parser -------------------------------------------------------------------- */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t'))
    {
        p++;
    }

    return p;
}

/** One identifier token into @p out (truncated); returns end position. */
static const char *token(const char *p, const char *end, char *out,
                         size_t cap)
{
    size_t w = 0;

    while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
    {
        if (w < cap - 1)
        {
            out[w++] = *p;
        }

        p++;
    }

    out[w] = '\0';
    return p;
}

/** Parse one `SG_ …` line body (after "SG_"). @return true on success. */
static bool parse_sg(const char *p, const char *end, ap_dbc_sig_t *s)
{
    char tok[8];

    memset(s, 0, sizeof(*s));
    s->factor = 1.0;
    p = skip_ws(p, end);
    p = token(p, end, s->name, sizeof(s->name));

    if (s->name[0] == '\0')
    {
        return false;
    }

    p = skip_ws(p, end);

    /* optional multiplexer indicator: M (switch), m<N> (multiplexed) or
       m<N>M (extended: a multiplexed multiplexor) */
    if (p < end && (*p == 'm' || *p == 'M') && *(p - 1) == ' ')
    {
        const char *q = token(p, end, tok, sizeof(tok));

        if (q < end && strcmp(tok, "M") == 0)
        {
            s->mux = 2;
            p = skip_ws(q, end);
        }
        else if (tok[0] == 'm' && isdigit((unsigned char)tok[1]))
        {
            char *ep;
            unsigned long v = strtoul(tok + 1, &ep, 10);

            if (*ep == 'M' || v > UINT16_MAX)
            {
                s->mux = 3;     /* extended — listed, never compiled */
            }
            else
            {
                s->mux = 1;
                s->mux_val = (uint16_t)v;
            }

            p = skip_ws(q, end);
        }
    }

    if (p >= end || *p != ':')
    {
        return false;
    }

    p = skip_ws(p + 1, end);

    /* <start>|<len>@<order><sign> */
    char *ep;
    long start = strtol(p, &ep, 10);

    if (ep == p || *ep != '|')
    {
        return false;
    }

    p = ep + 1;

    long len = strtol(p, &ep, 10);

    if (ep == p || *ep != '@' || start < 0 || start > 511 || len < 1 ||
        len > 64)
    {
        return false;
    }

    p = ep + 1;

    if (p >= end || (*p != '0' && *p != '1'))
    {
        return false;
    }

    s->intel = (*p == '1');
    p++;

    if (p >= end || (*p != '+' && *p != '-'))
    {
        return false;
    }

    s->is_signed = (*p == '-');
    s->start = (uint16_t)start;
    s->len = (uint8_t)len;
    p = skip_ws(p + 1, end);

    /* (factor,offset) */
    if (p < end && *p == '(')
    {
        s->factor = strtod(p + 1, &ep);

        if (*ep == ',')
        {
            s->offset = strtod(ep + 1, &ep);
        }

        if (*ep != ')')
        {
            return false;
        }

        p = skip_ws(ep + 1, end);
    }

    /* [min|max] */
    if (p < end && *p == '[')
    {
        s->min = strtod(p + 1, &ep);

        if (*ep == '|')
        {
            s->max = strtod(ep + 1, &ep);
        }

        if (*ep != ']')
        {
            return false;
        }

        p = skip_ws(ep + 1, end);
    }

    /* "unit" */
    if (p < end && *p == '"')
    {
        size_t w = 0;

        p++;

        while (p < end && *p != '"')
        {
            if (w < sizeof(s->unit) - 1)
            {
                s->unit[w++] = *p;
            }

            p++;
        }

        s->unit[w] = '\0';
    }

    return true;
}

int ap_dbc_parse(const char *text, size_t len, ap_dbc_msg_t *msgs,
                 size_t msgs_cap, int *n_msgs, ap_dbc_sig_t *sigs,
                 size_t sigs_cap, char *err, size_t err_len)
{
    const char *end = text + len;
    const char *line = text;
    int nm = 0, ns = 0;
    int cur_msg = -1;
    size_t rejected = 0;
    char first_reject[64] = "";

    if (text == NULL || len == 0)
    {
        snprintf(err, err_len, "empty file");
        return -1;
    }

    while (line < end)
    {
        const char *eol = line;

        while (eol < end && *eol != '\n')
        {
            eol++;
        }

        const char *p = skip_ws(line, eol);
        size_t ll = (size_t)(eol - p);

        if (ll > 4 && strncmp(p, "BO_ ", 4) == 0)
        {
            char *ep;
            unsigned long id = strtoul(p + 4, &ep, 10);
            ap_dbc_msg_t m;

            memset(&m, 0, sizeof(m));
            m.ext = (id & 0x80000000UL) != 0;
            m.id = (uint32_t)(id & 0x1FFFFFFFUL);
            p = skip_ws(ep, eol);
            p = token(p, eol, m.name, sizeof(m.name));

            if (p < eol && *p == ':')
            {
                m.dlc = (uint8_t)strtol(p + 1, NULL, 10);
            }

            /* Vector's pseudo-message for orphan signals */
            if ((id & 0x40000000UL) != 0 || m.name[0] == '\0')
            {
                cur_msg = -1;
            }
            else if ((size_t)nm < msgs_cap)
            {
                msgs[nm] = m;
                cur_msg = nm++;
            }
            else
            {
                cur_msg = -1;   /* over cap: keep what fits             */
            }
        }
        else if (ll > 4 && strncmp(p, "SG_ ", 4) == 0)
        {
            ap_dbc_sig_t s;

            if (cur_msg >= 0 && parse_sg(p + 4, eol, &s))
            {
                if ((size_t)ns < sigs_cap)
                {
                    s.msg = (uint16_t)cur_msg;

                    if (s.mux == 3)
                    {
                        msgs[cur_msg].mux_complex = true;
                    }
                    else if (s.mux == 2)
                    {
                        /* a second M switch in one message = extended */
                        for (int i = 0; i < ns; i++)
                        {
                            if (sigs[i].msg == (uint16_t)cur_msg &&
                                sigs[i].mux == 2)
                            {
                                msgs[cur_msg].mux_complex = true;
                                break;
                            }
                        }
                    }

                    sigs[ns++] = s;
                }
            }
            else
            {
                rejected++;

                if (first_reject[0] == '\0')
                {
                    size_t n = ll < sizeof(first_reject) - 1
                                   ? ll : sizeof(first_reject) - 1;

                    memcpy(first_reject, p, n);
                    first_reject[n] = '\0';
                }
            }
        }
        else if (ll > 12 && strncmp(p, "SG_MUL_VAL_ ", 12) == 0)
        {
            /* extended-multiplexing map — mark the message so its muxed
               signals stay listed-with-reason instead of decoding wrong */
            unsigned long id = strtoul(p + 12, NULL, 10) & 0x1FFFFFFFUL;

            for (int i = 0; i < nm; i++)
            {
                if (msgs[i].id == (uint32_t)id)
                {
                    msgs[i].mux_complex = true;
                    break;
                }
            }
        }
        else if (ll > 13 && strncmp(p, "SIG_VALTYPE_ ", 13) == 0)
        {
            /* SIG_VALTYPE_ <id> <sig> : <1|2>; -> float/double marker */
            char *ep;
            unsigned long id = strtoul(p + 13, &ep, 10) & 0x1FFFFFFFUL;
            char sname[AP_DBC_NAME_LEN];

            p = skip_ws(ep, eol);
            p = token(p, eol, sname, sizeof(sname));
            p = skip_ws(p, eol);

            if (p < eol && *p == ':')
            {
                int vt = (int)strtol(p + 1, NULL, 10);

                for (int i = 0; i < ns; i++)
                {
                    if (msgs[sigs[i].msg].id == (uint32_t)id &&
                        strcmp(sigs[i].name, sname) == 0)
                    {
                        sigs[i].valtype = (uint8_t)vt;
                    }
                }
            }
        }

        line = eol + 1;
    }

    if (ns == 0)
    {
        snprintf(err, err_len, "no signals found (%u SG_ rejected%s%s)",
                 (unsigned)rejected,
                 first_reject[0] ? ", first: " : "", first_reject);
        return -1;
    }

    *n_msgs = nm;
    return ns;
}

/* ---- signal -> expression compiler ---------------------------------------------- */

typedef struct
{
    uint8_t byte;               /* payload byte index                    */
    uint8_t shr;                /* right shift within the byte           */
    uint8_t bits;               /* bit count in this segment             */
    double  weight;             /* 2^n multiplier in the composed value  */
} dbc_seg_t;

/** Decompose the signal into per-byte segments (weight = LSB position
 *  of the segment inside the value). @return segment count, or -1. */
static int segments(const ap_dbc_sig_t *s, dbc_seg_t *seg, int cap)
{
    int n = 0;

    if (s->intel)
    {
        int lo = s->start, hi = s->start + s->len - 1;

        for (int k = lo / 8; k <= hi / 8 && n < cap; k++)
        {
            int sl = (lo > 8 * k) ? lo : 8 * k;
            int sh = (hi < 8 * k + 7) ? hi : 8 * k + 7;

            seg[n].byte = (uint8_t)k;
            seg[n].shr = (uint8_t)(sl - 8 * k);
            seg[n].bits = (uint8_t)(sh - sl + 1);
            seg[n].weight = ldexp(1.0, sl - lo);
            n++;
        }
    }
    else
    {
        /* Motorola: start = MSB position; linearize MSB-first          */
        int m0 = (s->start / 8) * 8 + (7 - s->start % 8);
        int m1 = m0 + s->len - 1;

        for (int k = m0 / 8; k <= m1 / 8 && n < cap; k++)
        {
            int hl = (m0 > 8 * k) ? m0 : 8 * k;          /* hi (MSB) lin */
            int ll = (m1 < 8 * k + 7) ? m1 : 8 * k + 7;  /* lo (LSB) lin */

            seg[n].byte = (uint8_t)k;
            seg[n].shr = (uint8_t)(7 - (ll - 8 * k));
            seg[n].bits = (uint8_t)(ll - hl + 1);
            seg[n].weight = ldexp(1.0, m1 - ll);
            n++;
        }
    }

    return n;
}

/** Emit one segment's extraction; @p op_out = contains operators. */
static void seg_text(const dbc_seg_t *g, char *out, size_t cap,
                     bool *has_ops)
{
    unsigned mask = (1u << g->bits) - 1;

    if (g->shr == 0 && g->bits == 8)
    {
        snprintf(out, cap, "B%u", g->byte);
        *has_ops = false;
    }
    else if (g->shr + g->bits == 8)
    {
        snprintf(out, cap, "B%u>>%u", g->byte, g->shr);
        *has_ops = true;
    }
    else if (g->shr == 0)
    {
        snprintf(out, cap, "B%u&%u", g->byte, mask);
        *has_ops = true;
    }
    else
    {
        snprintf(out, cap, "B%u>>%u&%u", g->byte, g->shr, mask);
        *has_ops = true;
    }
}

static bool append(char *out, size_t cap, size_t *w, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);

    int n = vsnprintf(out + *w, cap - *w, fmt, ap);

    va_end(ap);

    if (n < 0 || (size_t)n >= cap - *w)
    {
        return false;
    }

    *w += (size_t)n;
    return true;
}

esp_err_t ap_dbc_expr(const ap_dbc_sig_t *s, char *out, size_t out_cap,
                      const char **reason)
{
    static const char *R_MUXEXT = "extended multiplexing";
    static const char *R_FLOAT = "float/double signal";
    static const char *R_LONG = "compiled expression too long";
    char raw[AP_EXPR_LEN * 2];
    size_t w = 0;
    bool raw_ops = false;

    *reason = NULL;

    /* simple mux (M switch / m<N>) compiles like a plain signal — the
       m<N> gate is a SEPARATE runner precondition (ap_dbc_mux_cond) */
    if (s->mux == 3)
    {
        *reason = R_MUXEXT;
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s->valtype != 0)
    {
        *reason = R_FLOAT;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Motorola byte-aligned -> compact spans */
    bool span_ok = !s->intel && s->start % 8 == 7 && s->len % 8 == 0;
    int nbytes = s->len / 8;

    if (span_ok && (!s->is_signed || nbytes == 1 || nbytes == 2 ||
                    nbytes == 4 || nbytes == 8))
    {
        int f = s->start / 8, l = f + nbytes - 1;
        char c = s->is_signed ? 'S' : 'B';

        if (nbytes == 1)
        {
            append(raw, sizeof(raw), &w, "%c%d", c, f);
        }
        else
        {
            append(raw, sizeof(raw), &w, "[%c%d:%c%d]", c, f, c, l);
        }

        raw_ops = false;
    }
    else
    {
        dbc_seg_t seg[9];
        int n = segments(s, seg, 9);

        if (n <= 0)
        {
            *reason = R_LONG;
            return ESP_ERR_NOT_SUPPORTED;
        }

        for (int i = 0; i < n; i++)
        {
            char st[32], wt[24];
            bool ops = false;

            seg_text(&seg[i], st, sizeof(st), &ops);

            if (i > 0)
            {
                append(raw, sizeof(raw), &w, "+");
            }

            if (seg[i].weight > 1.0)
            {
                fmt_int(seg[i].weight, wt, sizeof(wt));
                append(raw, sizeof(raw), &w, ops ? "(%s)*%s" : "%s*%s",
                       st, wt);
            }
            else if (ops && n > 1)
            {
                /* '+' binds tighter than '>>' in the grammar — a bare
                 * shift segment inside a sum needs parens */
                append(raw, sizeof(raw), &w, "(%s)", st);
            }
            else
            {
                append(raw, sizeof(raw), &w, "%s", st);
            }
        }

        raw_ops = strpbrk(raw, ">&*+-|") != NULL;

        /* sign extension: RAW - msb*2^len (multiply-add — no 32-bit
         * ceiling; int ops only ever touch one byte) */
        if (s->is_signed)
        {
            int msb_byte, msb_bit;

            if (s->intel)
            {
                int m = s->start + s->len - 1;

                msb_byte = m / 8;
                msb_bit = m % 8;
            }
            else
            {
                msb_byte = s->start / 8;
                msb_bit = s->start % 8;
            }

            char p2[24];

            fmt_int(ldexp(1.0, s->len), p2, sizeof(p2));

            if (msb_bit == 7)
            {
                append(raw, sizeof(raw), &w, "-(B%u>>7)*%s", msb_byte,
                       p2);
            }
            else
            {
                append(raw, sizeof(raw), &w, "-(B%u>>%u&1)*%s", msb_byte,
                       msb_bit, p2);
            }

            raw_ops = true;
        }
    }

    /* scale */
    char expr[AP_EXPR_LEN * 2];
    size_t ew = 0;
    bool have_factor = (s->factor != 1.0);
    bool have_offset = (s->offset != 0.0);

    if (have_factor)
    {
        char f[32];

        fmt_num(s->factor, f, sizeof(f));

        if (!append(expr, sizeof(expr), &ew, raw_ops ? "(%s)*%s"
                                                     : "%s*%s", raw, f))
        {
            *reason = R_LONG;
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    else
    {
        if (!append(expr, sizeof(expr), &ew, "%s", raw))
        {
            *reason = R_LONG;
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    if (have_offset)
    {
        char o[32];

        if (s->offset >= 0)
        {
            fmt_num(s->offset, o, sizeof(o));

            if (!append(expr, sizeof(expr), &ew, "+%s", o))
            {
                *reason = R_LONG;
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
        else
        {
            fmt_num(-s->offset, o, sizeof(o));

            if (!append(expr, sizeof(expr), &ew, "-%s", o))
            {
                *reason = R_LONG;
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
    }

    if (ew >= out_cap)
    {
        *reason = R_LONG;
        return ESP_ERR_NOT_SUPPORTED;
    }

    memcpy(out, expr, ew + 1);
    return ESP_OK;
}

esp_err_t ap_dbc_mux_cond(const ap_dbc_sig_t *s, const ap_dbc_msg_t *msgs,
                          const ap_dbc_sig_t *sigs, int n_sigs,
                          char *expr_out, size_t expr_cap,
                          float *val_out, const char **reason)
{
    static const char *R_MUXEXT = "extended multiplexing";
    static const char *R_NOSW = "no multiplexer switch in message";
    static const char *R_SWLONG = "multiplexer switch too complex";

    *reason = NULL;
    expr_out[0] = '\0';
    *val_out = 0;

    if (s->mux == 3 || msgs[s->msg].mux_complex)
    {
        *reason = R_MUXEXT;
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s->mux != 1)
    {
        return ESP_OK;          /* plain / switch — unconditional        */
    }

    const ap_dbc_sig_t *sw = NULL;

    for (int i = 0; i < n_sigs; i++)
    {
        if (sigs[i].msg == s->msg && sigs[i].mux == 2)
        {
            sw = &sigs[i];
            break;
        }
    }

    if (sw == NULL)
    {
        *reason = R_NOSW;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* the DBC m<N> comparison is on the RAW switch value — compile the
       switch slice unscaled and unsigned */
    ap_dbc_sig_t raw_sw = *sw;

    raw_sw.mux = 0;
    raw_sw.valtype = 0;
    raw_sw.is_signed = false;
    raw_sw.factor = 1.0;
    raw_sw.offset = 0.0;

    const char *sub = NULL;

    if (ap_dbc_expr(&raw_sw, expr_out, expr_cap, &sub) != ESP_OK)
    {
        expr_out[0] = '\0';
        *reason = R_SWLONG;
        return ESP_ERR_NOT_SUPPORTED;
    }

    *val_out = (float)s->mux_val;
    return ESP_OK;
}

/** Reference decoder for the host cross-check (and nothing else). */
double ap_dbc_decode_ref(const ap_dbc_sig_t *s, const uint8_t data[8])
{
    uint64_t raw = 0;

    if (s->intel)
    {
        for (int i = s->len - 1; i >= 0; i--)
        {
            int b = s->start + i;

            raw = (raw << 1) | ((data[b / 8] >> (b % 8)) & 1u);
        }
    }
    else
    {
        int m0 = (s->start / 8) * 8 + (7 - s->start % 8);

        for (int i = 0; i < s->len; i++)
        {
            int lin = m0 + i;
            int bit = 7 - (lin % 8);

            raw = (raw << 1) | ((data[lin / 8] >> bit) & 1u);
        }
    }

    double v;

    if (s->is_signed && s->len < 64 && (raw & (1ULL << (s->len - 1))))
    {
        v = (double)(int64_t)(raw - (1ULL << s->len));
    }
    else
    {
        v = (double)raw;
    }

    return v * s->factor + s->offset;
}
