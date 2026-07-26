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
 * @file autopid_dtc_db_codec.c
 * @brief PURE DTC-database importer (TASK_dtc_db.md §2, host-tested):
 *        sniff + parse the wild formats (CSV/TSV/semicolon with quotes
 *        and headers, JSON map, JSON array-of-objects with key aliases,
 *        plain text) and normalize into the ONE canonical stored form:
 *
 *            #dtcdb1 <count>\n
 *            CODE\tDESCRIPTION\n        (sorted by code, deduped)
 *
 * The importer owns no memory: the caller provides the scratch buffer
 * (unsorted parsed lines), the item index, and the output buffer —
 * PSRAM heap on target, small statics in the host suite.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- entry emit (into scratch) ------------------------------------------------ */

typedef struct
{
    char  *scratch;
    size_t cap;
    size_t used;
    ap_dtc_db_item_t *items;
    size_t items_cap;
    size_t n;
    size_t rejected;
    char   first_reject[48];
} db_parse_t;

/** Validate + normalize one (code, desc) pair into scratch/items.
 *  Codes may carry UDS-style suffixes ("P0420-00") — truncated to the
 *  5-char base in v1 (TASK_dtc_db §7.3). */
static void emit_entry(db_parse_t *p, const char *code, size_t code_len,
                       const char *desc, size_t desc_len)
{
    uint8_t hi, lo;
    char base[AP_DTC_CODE_LEN];

    if (code_len > 5 &&
        (code[5] == '-' || code[5] == ' ' || code[5] == ':'))
    {
        code_len = 5;               /* strip sub-type suffix            */
    }

    if (code_len != 5)
    {
        goto reject;
    }

    memcpy(base, code, 5);
    base[5] = '\0';

    if (!ap_dtc_unformat(base, &hi, &lo))
    {
        goto reject;
    }

    ap_dtc_format(hi, lo, base);    /* canonical upper-case             */

    /* description: strip control chars, collapse to <= AP_DTC_DESC_MAX */
    if (p->n >= p->items_cap ||
        p->used + AP_DTC_DESC_MAX + 2 > p->cap)
    {
        p->rejected++;              /* over caps: keep what fits        */
        return;
    }

    ap_dtc_db_item_t *it = &p->items[p->n];

    memcpy(it->code, base, AP_DTC_CODE_LEN);
    it->off = (uint32_t)p->used;

    size_t w = 0;

    for (size_t i = 0; i < desc_len && w < AP_DTC_DESC_MAX; i++)
    {
        unsigned char c = (unsigned char)desc[i];

        if (c == '\t')
        {
            c = ' ';
        }

        if (c >= 0x20 || c >= 0x80)  /* printable ASCII + UTF-8 bytes   */
        {
            p->scratch[p->used + w++] = (char)c;
        }
    }

    /* trim trailing spaces */
    while (w > 0 && p->scratch[it->off + w - 1] == ' ')
    {
        w--;
    }

    it->len = (uint16_t)w;
    it->seq = (uint32_t)p->n;
    p->used += w;
    p->n++;
    return;

reject:
    p->rejected++;

    if (p->first_reject[0] == '\0' && code_len > 0)
    {
        size_t n = (code_len < sizeof(p->first_reject) - 1)
                       ? code_len : sizeof(p->first_reject) - 1;

        memcpy(p->first_reject, code, n);
        p->first_reject[n] = '\0';
    }
}

/* ---- minimal JSON scanner (narrow: our two shapes only) ------------------------ */

static const char *json_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' ||
                       *p == '\n'))
    {
        p++;
    }

    return p;
}

/** Parse a JSON string starting at the opening quote; unescape into
 *  @p out (cap @p out_cap). @return position after the closing quote,
 *  or NULL on malformed input. */
static const char *json_string(const char *p, const char *end, char *out,
                               size_t out_cap, size_t *out_len)
{
    size_t w = 0;

    if (p >= end || *p != '"')
    {
        return NULL;
    }

    p++;

    while (p < end && *p != '"')
    {
        char c = *p;

        if (c == '\\' && p + 1 < end)
        {
            p++;
            switch (*p)
            {
                case 'n': c = ' '; break;      /* newlines -> space     */
                case 't': c = ' '; break;
                case 'r': c = ' '; break;
                case 'b': case 'f': c = ' '; break;
                case 'u':
                    /* \uXXXX: ASCII range decoded, the rest -> '?' */
                    if (p + 4 < end)
                    {
                        unsigned v = 0;

                        for (int i = 1; i <= 4; i++)
                        {
                            char h = (char)tolower((unsigned char)p[i]);

                            v <<= 4;

                            if (h >= '0' && h <= '9')
                            {
                                v |= (unsigned)(h - '0');
                            }
                            else if (h >= 'a' && h <= 'f')
                            {
                                v |= (unsigned)(h - 'a' + 10);
                            }
                            else
                            {
                                v = '?';
                                break;
                            }
                        }

                        c = (v >= 0x20 && v < 0x7F) ? (char)v : '?';
                        p += 4;
                    }
                    break;
                default: c = *p; break;        /* \" \\ \/              */
            }
        }

        if (w < out_cap - 1)
        {
            out[w++] = c;
        }

        p++;
    }

    if (p >= end)
    {
        return NULL;
    }

    out[w] = '\0';

    if (out_len != NULL)
    {
        *out_len = w;
    }

    return p + 1;
}

/** Skip any JSON value (nested ok). @return position after it, NULL on
 *  malformed. */
static const char *json_skip(const char *p, const char *end)
{
    int depth = 0;

    p = json_ws(p, end);

    do
    {
        if (p >= end)
        {
            return NULL;
        }

        if (*p == '"')
        {
            char sink[2];

            p = json_string(p, end, sink, sizeof(sink), NULL);

            if (p == NULL)
            {
                return NULL;
            }
        }
        else if (*p == '{' || *p == '[')
        {
            depth++;
            p++;
        }
        else if (*p == '}' || *p == ']')
        {
            depth--;
            p++;
        }
        else
        {
            /* number / true / false / null */
            while (p < end && *p != ',' && *p != '}' && *p != ']' &&
                   *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t')
            {
                p++;
            }
        }

        p = json_ws(p, end);

        if (depth > 0 && p < end && *p == ',')
        {
            p++;
            p = json_ws(p, end);
        }
    } while (depth > 0);

    return p;
}

static bool key_is_code(const char *k)
{
    return strcasecmp(k, "code") == 0 || strcasecmp(k, "dtc") == 0 ||
           strcasecmp(k, "id") == 0;
}

static bool key_is_desc(const char *k)
{
    return strcasecmp(k, "description") == 0 ||
           strcasecmp(k, "desc") == 0 || strcasecmp(k, "text") == 0 ||
           strcasecmp(k, "meaning") == 0 || strcasecmp(k, "title") == 0;
}

/** {"P0420":"desc", ...} */
static bool parse_json_map(db_parse_t *p, const char *in, const char *end)
{
    char key[32], val[AP_DTC_DESC_MAX + 32];
    size_t klen, vlen;
    const char *q = json_ws(in, end);

    if (q >= end || *q != '{')
    {
        return false;
    }

    q = json_ws(q + 1, end);

    while (q < end && *q != '}')
    {
        q = json_string(q, end, key, sizeof(key), &klen);

        if (q == NULL)
        {
            return false;
        }

        q = json_ws(q, end);

        if (q >= end || *q != ':')
        {
            return false;
        }

        q = json_ws(q + 1, end);

        if (q < end && *q == '"')
        {
            q = json_string(q, end, val, sizeof(val), &vlen);

            if (q == NULL)
            {
                return false;
            }

            emit_entry(p, key, klen, val, vlen);
        }
        else
        {
            q = json_skip(q, end);   /* non-string value: not an entry  */

            if (q == NULL)
            {
                return false;
            }
        }

        q = json_ws(q, end);

        if (q < end && *q == ',')
        {
            q = json_ws(q + 1, end);
        }
    }

    return q < end;
}

/** [{"code":"P0420","description":"..."}, ...] with key aliases. */
static bool parse_json_array(db_parse_t *p, const char *in,
                             const char *end)
{
    const char *q = json_ws(in, end);

    if (q >= end || *q != '[')
    {
        return false;
    }

    q = json_ws(q + 1, end);

    while (q < end && *q != ']')
    {
        if (*q != '{')
        {
            q = json_skip(q, end);   /* non-object element              */

            if (q == NULL)
            {
                return false;
            }
        }
        else
        {
            char key[32], val[AP_DTC_DESC_MAX + 32];
            char code[32] = "", desc[AP_DTC_DESC_MAX + 32] = "";
            size_t klen, vlen, code_len = 0, desc_len = 0;

            q = json_ws(q + 1, end);

            while (q < end && *q != '}')
            {
                q = json_string(q, end, key, sizeof(key), &klen);

                if (q == NULL)
                {
                    return false;
                }

                q = json_ws(q, end);

                if (q >= end || *q != ':')
                {
                    return false;
                }

                q = json_ws(q + 1, end);

                if (q < end && *q == '"')
                {
                    q = json_string(q, end, val, sizeof(val), &vlen);

                    if (q == NULL)
                    {
                        return false;
                    }

                    if (key_is_code(key))
                    {
                        code_len = vlen < sizeof(code) - 1
                                       ? vlen : sizeof(code) - 1;
                        memcpy(code, val, code_len);
                        code[code_len] = '\0';
                    }
                    else if (key_is_desc(key))
                    {
                        desc_len = vlen < sizeof(desc) - 1
                                       ? vlen : sizeof(desc) - 1;
                        memcpy(desc, val, desc_len);
                        desc[desc_len] = '\0';
                    }
                }
                else
                {
                    q = json_skip(q, end);

                    if (q == NULL)
                    {
                        return false;
                    }
                }

                q = json_ws(q, end);

                if (q < end && *q == ',')
                {
                    q = json_ws(q + 1, end);
                }
            }

            if (q >= end)
            {
                return false;
            }

            q++;                     /* past '}'                        */

            if (code_len > 0)
            {
                emit_entry(p, code, code_len, desc, desc_len);
            }
        }

        q = json_ws(q, end);

        if (q < end && *q == ',')
        {
            q = json_ws(q + 1, end);
        }
    }

    return q < end;
}

/* ---- delimited / plain text ---------------------------------------------------- */

/** Unquote a possibly-"quoted" field in place-ish: returns start/len of
 *  the content, doubled quotes collapsed into @p tmp when needed. */
static const char *field_content(const char *f, size_t flen, char *tmp,
                                 size_t tmp_cap, size_t *out_len)
{
    /* trim */
    while (flen > 0 && (f[0] == ' ' || f[0] == '\r'))
    {
        f++;
        flen--;
    }

    while (flen > 0 && (f[flen - 1] == ' ' || f[flen - 1] == '\r'))
    {
        flen--;
    }

    if (flen >= 2 && f[0] == '"' && f[flen - 1] == '"')
    {
        size_t w = 0;

        for (size_t i = 1; i + 1 < flen && w < tmp_cap - 1; i++)
        {
            if (f[i] == '"' && f[i + 1] == '"')
            {
                i++;                 /* "" -> "                         */
            }

            tmp[w++] = f[i];
        }

        *out_len = w;
        return tmp;
    }

    *out_len = flen;
    return f;
}

/** One text line -> (code, desc). Delimiter = first of TAB/;/, outside
 *  quotes; else the first whitespace run (plain shape). */
static void parse_line(db_parse_t *p, const char *line, size_t len,
                       char *delim_seen)
{
    /* comments/blank */
    while (len > 0 && (line[0] == ' ' || line[0] == '\r'))
    {
        line++;
        len--;
    }

    if (len == 0 || line[0] == '#')
    {
        return;
    }

    size_t split = 0;
    char d = '\0';
    bool in_q = false;

    for (size_t i = 0; i < len; i++)
    {
        if (line[i] == '"')
        {
            in_q = !in_q;
        }
        else if (!in_q && (line[i] == '\t' || line[i] == ';' ||
                           line[i] == ','))
        {
            split = i;
            d = line[i];
            break;
        }
    }

    if (d == '\0')
    {
        for (size_t i = 0; i < len; i++)
        {
            if (line[i] == ' ')
            {
                split = i;
                d = ' ';
                break;
            }
        }
    }

    char ctmp[40], dtmp[AP_DTC_DESC_MAX + 8];
    size_t clen, dlen;
    const char *code, *desc;

    if (d == '\0')
    {
        code = field_content(line, len, ctmp, sizeof(ctmp), &clen);
        desc = "";
        dlen = 0;
    }
    else
    {
        code = field_content(line, split, ctmp, sizeof(ctmp), &clen);

        const char *rest = line + split + 1;
        size_t rest_len = len - split - 1;

        if (d == ' ')
        {
            while (rest_len > 0 && rest[0] == ' ')
            {
                rest++;
                rest_len--;
            }
        }

        desc = field_content(rest, rest_len, dtmp, sizeof(dtmp), &dlen);
    }

    size_t before = p->n;

    emit_entry(p, code, clen, desc, dlen);

    if (p->n > before && *delim_seen == '\0')
    {
        *delim_seen = (d == '\0') ? ' ' : d;
    }
}

/* ---- sort + dedup + serialize --------------------------------------------------- */

static int item_cmp(const void *a, const void *b)
{
    const ap_dtc_db_item_t *ia = a, *ib = b;
    int c = memcmp(ia->code, ib->code, 5);

    if (c != 0)
    {
        return c;
    }

    /* equal codes: LAST occurrence wins -> sort earlier seq first, the
     * dedup pass keeps the final one of each run */
    return (ia->seq < ib->seq) ? -1 : 1;
}

int ap_dtc_db_import(const char *in, size_t in_len, char *scratch,
                     size_t scratch_cap, ap_dtc_db_item_t *items,
                     size_t items_cap, char fmt_out[12], char *err,
                     size_t err_len)
{
    db_parse_t p = { .scratch = scratch, .cap = scratch_cap,
                     .items = items, .items_cap = items_cap };
    const char *end = in + in_len;
    const char *q = json_ws(in, end);
    char delim = '\0';

    fmt_out[0] = '\0';

    if (in == NULL || in_len == 0)
    {
        snprintf(err, err_len, "empty file");
        return -1;
    }

    if (q < end && *q == '{')
    {
        snprintf(fmt_out, 12, "json-map");

        if (!parse_json_map(&p, in, end))
        {
            snprintf(err, err_len, "malformed JSON object");
            return -1;
        }
    }
    else if (q < end && *q == '[')
    {
        snprintf(fmt_out, 12, "json-array");

        if (!parse_json_array(&p, in, end))
        {
            snprintf(err, err_len, "malformed JSON array");
            return -1;
        }
    }
    else
    {
        const char *line = in;

        while (line < end)
        {
            const char *eol = line;

            while (eol < end && *eol != '\n')
            {
                eol++;
            }

            parse_line(&p, line, (size_t)(eol - line), &delim);
            line = eol + 1;
        }

        snprintf(fmt_out, 12, "%s",
                 (delim == '\t') ? "tsv"
                 : (delim == ';') ? "semicolon"
                 : (delim == ',') ? "csv" : "text");
    }

    if (p.n == 0)
    {
        snprintf(err, err_len, "no valid entries (%u rejected%s%s)",
                 (unsigned)p.rejected,
                 p.first_reject[0] ? ", first: " : "", p.first_reject);
        return -1;
    }

    qsort(items, p.n, sizeof(items[0]), item_cmp);

    /* dedup: keep the LAST of each equal-code run (highest seq = last
     * in the sorted run thanks to the tiebreak) */
    size_t w = 0;

    for (size_t i = 0; i < p.n; i++)
    {
        if (i + 1 < p.n && memcmp(items[i].code, items[i + 1].code, 5)
                               == 0)
        {
            continue;
        }

        items[w++] = items[i];
    }

    return (int)w;
}

size_t ap_dtc_db_serialize(const char *scratch,
                           const ap_dtc_db_item_t *items, int n,
                           char *out, size_t out_cap)
{
    size_t w = (size_t)snprintf(out, out_cap, "#dtcdb1 %d\n", n);

    for (int i = 0; i < n; i++)
    {
        size_t need = 5 + 1 + items[i].len + 1;

        if (w + need >= out_cap)
        {
            return 0;               /* caller sized out too small       */
        }

        memcpy(out + w, items[i].code, 5);
        w += 5;
        out[w++] = '\t';
        memcpy(out + w, scratch + items[i].off, items[i].len);
        w += items[i].len;
        out[w++] = '\n';
    }

    return w;
}

int ap_dtc_db_index(const char *buf, size_t len,
                    ap_dtc_db_item_t *items, size_t items_cap)
{
    const char *end = buf + len;
    const char *line = buf;
    int declared = -1;
    size_t n = 0;

    /* header */
    if (len < 8 || strncmp(buf, "#dtcdb1 ", 8) != 0)
    {
        return -1;
    }

    declared = atoi(buf + 8);

    while (line < end && *line != '\n')
    {
        line++;
    }

    line++;

    while (line < end && n < items_cap)
    {
        const char *eol = line;

        while (eol < end && *eol != '\n')
        {
            eol++;
        }

        if (eol - line >= 6 && line[5] == '\t')
        {
            memcpy(items[n].code, line, 5);
            items[n].code[5] = '\0';
            items[n].off = (uint32_t)(line + 6 - buf);
            items[n].len = (uint16_t)(eol - line - 6);
            items[n].seq = (uint32_t)n;
            n++;
        }

        line = eol + 1;
    }

    return ((int)n == declared) ? (int)n : -1;
}

bool ap_dtc_db_match(const char *code, const char *desc, size_t desc_len,
                     const char *q)
{
    size_t qlen;

    if (q == NULL || q[0] == '\0')
    {
        return true;
    }

    qlen = strlen(q);

    /* code prefix, case-insensitive */
    if (qlen <= 5)
    {
        size_t i = 0;

        while (i < qlen &&
               toupper((unsigned char)q[i]) == (unsigned char)code[i])
        {
            i++;
        }

        if (i == qlen)
        {
            return true;
        }
    }

    /* description substring, case-insensitive */
    if (qlen > desc_len)
    {
        return false;
    }

    for (size_t i = 0; i + qlen <= desc_len; i++)
    {
        size_t j = 0;

        while (j < qlen && tolower((unsigned char)desc[i + j]) ==
                               tolower((unsigned char)q[j]))
        {
            j++;
        }

        if (j == qlen)
        {
            return true;
        }
    }

    return false;
}
