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
 * @file script_engine_flash_bind.c
 * @brief The flash_* bindings: bytes in, bytes out, so a script can run an
 *        ECU's programming sequence (UDS 34/36/37 or a native loader) with
 *        the per-frame work in C. Documented in script_engine_doc_flash.c.
 *
 * Everything that transmits needs the allow_reflash setting and takes the
 * exclusive-bus hold. flash_commit() marks the point of no return: from
 * then the device is locked (script_engine_flash.c) and the run's
 * max_runtime_ms no longer applies.
 */
#include "script_engine_flash.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "script_engine_obd.h"
#include "script_engine_private.h"

#define FL_ERR        "flash_error"

static void need_reflash(bvm *vm)
{
    se_check_budget(vm);

    if (!se_settings_allow_reflash())
    {
        be_raise(vm, FL_ERR, "needs Allow reflash (script_engine.allow_reflash)");
    }

    se_bus_hold();
}

const uint8_t *se_fl_bytes(bvm *vm, int idx, size_t *len)
{
    if (be_top(vm) < idx || !be_isbytes(vm, idx))
    {
        be_raise(vm, FL_ERR, "expected bytes");
    }

    return be_tobytes(vm, idx, len);
}

uint32_t se_fl_range(bvm *vm, int idx, uint32_t def, uint32_t max,
                     const char *what)
{
    if (be_top(vm) < idx || be_isnil(vm, idx))
    {
        return def;
    }

    if (!be_isint(vm, idx) || be_toint(vm, idx) < 0 ||
        be_toint(vm, idx) > (bint)max)
    {
        be_raise(vm, FL_ERR, what);
    }

    return (uint32_t)be_toint(vm, idx);
}

uint32_t se_fl_int(bvm *vm, int idx, uint32_t def)
{
    return se_fl_range(vm, idx, def, UINT32_MAX, "expected an int 0..2^32-1");
}

/* a wait in ms: 0 (look once) .. 1 h */
static uint32_t ms_arg(bvm *vm, int idx, uint32_t def)
{
    return se_fl_range(vm, idx, def, 3600000, "a timeout is 0..3600000 ms");
}

const char *se_fl_str(bvm *vm, int idx, const char *what)
{
    if (be_top(vm) < idx || !be_isstring(vm, idx))
    {
        be_raise(vm, FL_ERR, what);
    }

    return be_tostring(vm, idx);
}

/* opts[key] from a map at @p idx (0: none), as an int; @p def if absent. */
typedef enum
{
    OPT_ABSENT = 0,
    OPT_INT,
    OPT_BOOL,
} opt_kind_t;

/* opts[key] from the map at @p idx (absent or nil: no options). An int or
   a bool comes back in *v (a bool as 0/1); any other value raises. */
static opt_kind_t opt_get(bvm *vm, int idx, const char *key, bint *v)
{
    if (be_top(vm) < idx || be_isnil(vm, idx))
    {
        return OPT_ABSENT;
    }

    if (!be_ismapinstance(vm, idx))
    {
        be_raise(vm, FL_ERR, "options are a map, e.g. {'pad': 0xAA}");
    }

    be_getmember(vm, idx, ".p");
    be_pushstring(vm, key);
    be_getindex(vm, -2);

    opt_kind_t kind = OPT_ABSENT;

    if (be_isint(vm, -1))
    {
        kind = OPT_INT;
        *v = be_toint(vm, -1);
    }
    else if (be_isbool(vm, -1))
    {
        kind = OPT_BOOL;
        *v = be_tobool(vm, -1) ? 1 : 0;
    }
    else if (!be_isnil(vm, -1))
    {
        be_pop(vm, 3);
        be_raise(vm, FL_ERR, key); /* names the option of the wrong type */
    }

    be_pop(vm, 3);
    return kind;
}

/* An int option in min..max, @p def when absent; a bool or a value out of
   range raises with @p what. */
static bint opt_int(bvm *vm, int idx, const char *key, bint def, bint min,
                    bint max, const char *what)
{
    bint v = def;
    opt_kind_t k = opt_get(vm, idx, key, &v);

    if (k == OPT_ABSENT)
    {
        return def;
    }

    if (k != OPT_INT || v < min || v > max)
    {
        be_raise(vm, FL_ERR, what);
    }

    return v;
}

/* A yes/no option: a bool, or an int (0 = no). */
static bool opt_flag(bvm *vm, int idx, const char *key)
{
    bint v = 0;

    return opt_get(vm, idx, key, &v) != OPT_ABSENT && v != 0;
}

/* The CAN ids at @p idx: absent or nil = every id (returns 0), one int =
   that id, a list = 1..cap ints. Anything else raises: a filter that
   silently became "every id" would read the wrong frames. */
static size_t ids_arg(bvm *vm, int idx, uint32_t *ids, size_t cap)
{
    if (be_top(vm) < idx || be_isnil(vm, idx))
    {
        return 0;
    }

    if (be_isint(vm, idx))
    {
        ids[0] = (uint32_t)be_toint(vm, idx);
        return 1;
    }

    if (!be_islistinstance(vm, idx))
    {
        be_raise(vm, FL_ERR, "ids: an int or a list of ints");
    }

    be_getmember(vm, idx, ".p");

    int size = be_data_size(vm, -1);

    if (size < 1 || (size_t)size > cap)
    {
        be_pop(vm, 1);
        be_raise(vm, FL_ERR, "ids: 1..16 entries");
    }

    for (int i = 0; i < size; i++)
    {
        be_pushint(vm, i);
        be_getindex(vm, -2);

        if (!be_isint(vm, -1) || be_toint(vm, -1) < 0 ||
            be_toint(vm, -1) > 0x1FFFFFFF)
        {
            be_pop(vm, 2);
            be_raise(vm, FL_ERR, "ids: ints 0..0x1FFFFFFF only");
        }

        ids[i] = (uint32_t)be_toint(vm, -1);
        be_pop(vm, 1);
    }

    be_pop(vm, 1);
    return (size_t)size;
}

static int handle_arg(bvm *vm)
{
    if (be_top(vm) < 1 || !be_isint(vm, 1) || be_toint(vm, 1) < 0 ||
        be_toint(vm, 1) >= SE_TP_SESSIONS)
    {
        be_raise(vm, FL_ERR, "expected a session handle from flash_isotp()");
    }

    return (int)be_toint(vm, 1);
}

/* ---- ISO-TP sessions ---------------------------------------------------------- */

/* flash_isotp(tx, rx[, {ext, ae_tx, ae_rx, pad, bs, stmin, gap_us}]) -> h */
static int b_isotp(bvm *vm)
{
    need_reflash(vm);

    if (be_top(vm) < 2 || !be_isint(vm, 1) || !be_isint(vm, 2))
    {
        be_raise(vm, FL_ERR, "flash_isotp(tx_id, rx_id[, opts])");
    }

    bool ext = opt_flag(vm, 3, "ext");
    bint max_id = ext ? 0x1FFFFFFF : 0x7FF;

    if (be_toint(vm, 1) < 0 || be_toint(vm, 1) > max_id ||
        be_toint(vm, 2) < 0 || be_toint(vm, 2) > max_id)
    {
        be_raise(vm, FL_ERR, "an id is 0..0x7FF, or 0..0x1FFFFFFF with ext");
    }

    se_tp_opts_t o;

    se_tp_opts_default(&o);

    /* pad: a byte, -1 or false = no padding, true = padded with 0x00 */
    bint pad = 0;
    opt_kind_t pk = opt_get(vm, 3, "pad", &pad);

    if (pk == OPT_BOOL)
    {
        pad = pad ? 0 : -1;
    }
    else if (pk == OPT_INT && (pad < -1 || pad > 0xFF))
    {
        be_raise(vm, FL_ERR, "pad is a byte, -1 (or false) for none");
    }

    bint stmin = opt_int(vm, 3, "stmin", 0, 0, 0xF9, "stmin is 0..0x7F or 0xF1..0xF9");

    if (stmin > 0x7F && stmin < 0xF1)
    {
        be_raise(vm, FL_ERR, "stmin is 0..0x7F or 0xF1..0xF9");
    }

    o.ae_tx = (int16_t)opt_int(vm, 3, "ae_tx", -1, -1, 0xFF, "ae_tx is -1..0xFF");
    o.ae_rx = (int16_t)opt_int(vm, 3, "ae_rx", -1, -1, 0xFF, "ae_rx is -1..0xFF");
    o.pad = pad >= 0;
    o.pad_byte = (uint8_t)((pad >= 0) ? pad : 0);
    o.bs = (uint8_t)opt_int(vm, 3, "bs", 0, 0, 0xFF, "bs is 0..255");
    o.stmin = (uint8_t)stmin;

    /* a gap is a minimum between our CFs; 127 ms is ISO 15765-2's ceiling */
    uint32_t gap = (uint32_t)opt_int(vm, 3, "gap_us", 0, 0, 127000,
                                     "gap_us is 0..127000");
    int h = se_tp_open((uint32_t)be_toint(vm, 1), (uint32_t)be_toint(vm, 2),
                       ext, &o, gap);

    if (h < 0)
    {
        be_raise(vm, FL_ERR, "no free session (4 at most) or the bus is down");
    }

    be_pushint(vm, h);
    be_return(vm);
}

static int b_close(bvm *vm)
{
    se_tp_close(handle_arg(vm));
    be_return_nil(vm);
}

/* flash_send(h, bytes[, timeout_ms]) -> true */
static int b_send(bvm *vm)
{
    need_reflash(vm);

    int h = handle_arg(vm);
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 2, &n);
    esp_err_t err = se_tp_send(h, d, n, ms_arg(vm, 3, 1000));

    if (err != ESP_OK)
    {
        be_raise(vm, FL_ERR, esp_err_to_name(err));
    }

    be_pushbool(vm, true);
    be_return(vm);
}

/* flash_recv(h[, timeout_ms]) -> bytes, or nil */
static int b_recv(bvm *vm)
{
    se_check_budget(vm);

    int h = handle_arg(vm);
    const uint8_t *d = NULL;
    size_t n = 0;
    esp_err_t err = se_tp_recv(h, &d, &n, ms_arg(vm, 2, 1000));

    if (err == ESP_ERR_TIMEOUT)
    {
        be_return_nil(vm);
    }

    if (err != ESP_OK)
    {
        be_raise(vm, FL_ERR, esp_err_to_name(err));
    }

    be_pushbytes(vm, d, n);
    be_return(vm);
}

/* flash_request(h, bytes[, timeout_ms[, {pending_ms, retries, raw}]])
 * -> the reply; waits through 7F xx 78; a 7F reply raises "nrc" unless raw;
 * nil when nothing came (after the retries). */
static int b_request(bvm *vm)
{
    need_reflash(vm);

    int h = handle_arg(vm);
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 2, &n);
    uint32_t timeout = ms_arg(vm, 3, 1000);
    uint32_t pending = (uint32_t)opt_int(vm, 4, "pending_ms", 5000, 0, 600000,
                                         "pending_ms is 0..600000");
    int retries = (int)opt_int(vm, 4, "retries", 0, 0, 16, "retries is 0..16");
    bool raw = opt_flag(vm, 4, "raw");

    for (int attempt = 0; attempt <= retries; attempt++)
    {
        se_check_budget(vm); /* Stop and the budget, before every attempt */
        se_tp_flush(h);

        esp_err_t err = se_tp_send(h, d, n, timeout);

        if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
        {
            be_raise(vm, FL_ERR, esp_err_to_name(err));
        }

        const uint8_t *r = NULL;
        size_t rn = 0;
        uint32_t wait = timeout;

        while (err == ESP_OK &&
               (err = se_tp_recv(h, &r, &rn, wait)) == ESP_OK)
        {
            bool pending_reply = rn >= 3 && r[0] == 0x7F && r[2] == 0x78;

            if (!pending_reply)
            {
                if (!raw && rn >= 3 && r[0] == 0x7F)
                {
                    char msg[32];

                    snprintf(msg, sizeof(msg), "NRC 0x%02X for 0x%02X", r[2], r[1]);
                    be_raise(vm, "nrc", msg);
                }

                be_pushbytes(vm, r, rn);
                be_return(vm);
            }

            wait = pending;
            se_check_budget(vm);
        }

        if (err != ESP_ERR_TIMEOUT)
        {
            be_raise(vm, FL_ERR, esp_err_to_name(err));
        }
    }

    be_return_nil(vm);
}

/* ---- raw frames ---------------------------------------------------------------- */

/* flash_can_send(id, bytes[, ext]) -> true */
static int b_can_send(bvm *vm)
{
    need_reflash(vm);

    if (be_top(vm) < 2 || !be_isint(vm, 1))
    {
        be_raise(vm, FL_ERR, "flash_can_send(id, bytes[, ext])");
    }

    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 2, &n);
    bool ext = be_top(vm) >= 3 && be_tobool(vm, 3);

    if (n > 8)
    {
        be_raise(vm, FL_ERR, "a frame carries 8 bytes at most");
    }

    if (be_toint(vm, 1) < 0 || be_toint(vm, 1) > (ext ? 0x1FFFFFFF : 0x7FF))
    {
        be_raise(vm, FL_ERR, "an id is 0..0x7FF, or 0..0x1FFFFFFF with ext");
    }

    esp_err_t err = se_raw_send((uint32_t)be_toint(vm, 1), ext, d, (uint8_t)n);

    if (err != ESP_OK)
    {
        be_raise(vm, FL_ERR, esp_err_to_name(err));
    }

    be_pushbool(vm, true);
    be_return(vm);
}

/* flash_can_recv(timeout_ms[, [ids]]) -> [id, bytes], or nil */
static int b_can_recv(bvm *vm)
{
    se_check_budget(vm);

    uint32_t ids[16];
    size_t n = ids_arg(vm, 2, ids, 16);
    can_core_frame_t f;

    if (se_raw_recv(ids, n, ms_arg(vm, 1, 1000), &f) != ESP_OK)
    {
        be_return_nil(vm);
    }

    be_newobject(vm, "list");
    be_pushint(vm, (bint)f.id);
    be_data_push(vm, -2);
    be_pop(vm, 1);
    be_pushbytes(vm, f.data, f.dlc);
    be_data_push(vm, -2);
    be_pop(vm, 1);
    be_pop(vm, 1);
    be_return(vm);
}

/* flash_can_quiet(window_ms, timeout_ms[, [ids]]) -> bool */
static int b_can_quiet(bvm *vm)
{
    se_check_budget(vm);

    uint32_t ids[16];
    size_t n = ids_arg(vm, 3, ids, 16);

    be_pushbool(vm, se_raw_quiet(ids, n, ms_arg(vm, 1, 1000),
                                 ms_arg(vm, 2, 10000)));
    be_return(vm);
}

/* ---- the run ------------------------------------------------------------------- */

static int b_commit(bvm *vm)
{
    need_reflash(vm);

    /* past the commit only the runner may transmit: a claimed obd_*
       conversation (its frames go out from the transport's own task and
       its tester present from a timer) would break half-way */
    if (se_obd_claimed())
    {
        be_raise(vm, FL_ERR, "obd_release() before flash_commit(): only "
                 "flash_* bindings reach the bus after it");
    }

    const char *label = (be_top(vm) >= 1 && be_isstring(vm, 1))
                            ? be_tostring(vm, 1) : "flash";
    esp_err_t err = se_flash_commit(label);

    switch (err)
    {
        case ESP_OK:
            break;
        case ESP_ERR_NOT_SUPPORTED:
            be_raise(vm, FL_ERR, "flash_commit needs an async run "
                     "(POST /api/scripts/run with \"async\": true)");
            break;
        case ESP_ERR_INVALID_STATE:
            be_raise(vm, FL_ERR, "a firmware update of the WiCAN or its OBD "
                     "chip is running: not committed");
            break;
        case ESP_ERR_NOT_ALLOWED:
            be_raise(vm, "killed", "Stop was pressed: not committed");
            break;
        default:
            be_raise(vm, FL_ERR, "the commit could not be recorded: stop here");
            break;
    }

    be_pushbool(vm, true);
    be_return(vm);
}

/* ---- registration -------------------------------------------------------------- */

static const struct
{
    const char *name;
    bntvfunc    fn;
} FLASH_BINDINGS[] =
{
    { "flash_isotp",           b_isotp },
    { "flash_send",            b_send },
    { "flash_recv",            b_recv },
    { "flash_request",         b_request },
    { "flash_close",           b_close },
    { "flash_can_send",        b_can_send },
    { "flash_can_recv",        b_can_recv },
    { "flash_can_quiet",       b_can_quiet },
    { "flash_commit",          b_commit },
    { "flash_abort_requested", se_fr_abort_requested },
    { "flash_state",           se_fr_state },
    { "flash_phase",           se_fr_phase },
    { "flash_progress",        se_fr_progress },
    { "flash_log",             se_fr_log },
    { "flash_ask",             se_fr_ask },
    { "flash_param",           se_fr_param },
    { "flash_micros",          se_fr_micros },
    { "flash_file_size",       se_fb_file_size },
    { "flash_file_read",       se_fb_file_read },
    { "flash_file_write",      se_fb_file_write },
    { "flash_file_sha256",     se_fb_file_sha256 },
    { "flash_crc",             se_fb_crc },
    { "flash_adler32",         se_fb_adler32 },
    { "flash_sum",             se_fb_sum },
    { "flash_sha256",          se_fb_sha256 },
};

#define N_FLASH (sizeof(FLASH_BINDINGS) / sizeof(FLASH_BINDINGS[0]))

void se_flash_bindings_register(bvm *vm)
{
    for (size_t i = 0; i < N_FLASH; i++)
    {
        be_pushntvfunction(vm, FLASH_BINDINGS[i].fn);
        be_setglobal(vm, FLASH_BINDINGS[i].name);
        be_pop(vm, 1);
    }
}

size_t se_flash_binding_count(void)
{
    return N_FLASH;
}

const char *se_flash_binding_name(size_t i)
{
    return (i < N_FLASH) ? FLASH_BINDINGS[i].name : NULL;
}
