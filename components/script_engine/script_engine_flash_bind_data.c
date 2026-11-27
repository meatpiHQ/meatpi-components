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
 * @file script_engine_flash_bind_data.c
 * @brief The flash_* bindings on data: SD files (through flash_jobs, see
 *        script_engine_flash_file.c) and the checks. Split from
 *        script_engine_flash_bind.c for the 700-line rule; listed in its
 *        table.
 */
#include "script_engine_flash.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "psa/crypto.h"

#include "script_engine_private.h"

#define FL_ERR        "flash_error"
#define FL_READ_MAX   4096 /* flash_file_read: bytes per call              */

/* ---- SD files ------------------------------------------------------------------ */

static const char *path_arg(bvm *vm)
{
    const char *p = se_fl_str(vm, 1, "expected a /sd/... path");

    if (!se_file_path_ok(p))
    {
        be_raise(vm, FL_ERR, "only /sd/... paths, no \"..\"");
    }

    return p;
}

int se_fb_file_size(bvm *vm)
{
    se_check_budget(vm);

    size_t size = 0;

    if (se_file_size(path_arg(vm), &size) != ESP_OK)
    {
        be_return_nil(vm);
    }

    be_pushint(vm, (bint)size);
    be_return(vm);
}

EXT_RAM_BSS_ATTR static uint8_t s_read_buf[FL_READ_MAX]; /* runs serialize */

int se_fb_file_read(bvm *vm)
{
    se_check_budget(vm);

    const char *path = path_arg(vm);
    uint32_t off = se_fl_int(vm, 2, 0);
    uint32_t len = se_fl_int(vm, 3, 0);
    size_t got = 0;

    if (len == 0 || len > FL_READ_MAX)
    {
        be_raise(vm, FL_ERR, "flash_file_read(path, offset, 1..4096)");
    }

    if (se_file_read(path, off, s_read_buf, len, &got) != ESP_OK)
    {
        be_return_nil(vm);
    }

    be_pushbytes(vm, s_read_buf, got);
    be_return(vm);
}

int se_fb_file_write(bvm *vm)
{
    se_check_budget(vm);

    const char *path = path_arg(vm);
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 2, &n);
    bool append = be_top(vm) >= 3 && be_tobool(vm, 3);
    esp_err_t err = se_file_write(path, d, n, append);

    if (err != ESP_OK)
    {
        be_raise(vm, FL_ERR, esp_err_to_name(err));
    }

    be_pushbool(vm, true);
    be_return(vm);
}

int se_fb_file_sha256(bvm *vm)
{
    se_check_budget(vm);

    char hex[65];

    if (se_file_sha256(path_arg(vm), hex) != ESP_OK)
    {
        be_return_nil(vm);
    }

    be_pushstring(vm, hex);
    be_return(vm);
}

/* ---- checks -------------------------------------------------------------------- */

/* flash_crc(bytes, width, poly, init, refin, refout, xorout) -> int */
int se_fb_crc(bvm *vm)
{
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 1, &n);
    se_crc_spec_t spec =
    {
        .width  = (uint8_t)se_fl_range(vm, 2, 32, 32, "width is 1..32"),
        .poly   = se_fl_int(vm, 3, 0x04C11DB7),
        .init   = se_fl_int(vm, 4, 0xFFFFFFFF),
        .refin  = be_top(vm) >= 5 ? be_tobool(vm, 5) : true,
        .refout = be_top(vm) >= 6 ? be_tobool(vm, 6) : true,
        .xorout = se_fl_int(vm, 7, 0xFFFFFFFF),
    };
    uint32_t crc = 0;

    if (!se_crc(&spec, d, n, &crc))
    {
        be_raise(vm, FL_ERR, "width is 1..32");
    }

    be_pushint(vm, (bint)crc);
    be_return(vm);
}

int se_fb_adler32(bvm *vm)
{
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 1, &n);

    be_pushint(vm, (bint)se_adler32(se_fl_int(vm, 2, 1), d, n));
    be_return(vm);
}

int se_fb_sum(bvm *vm)
{
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 1, &n);
    uint32_t width = se_fl_int(vm, 2, 1);
    bool big = be_top(vm) >= 3 && be_tobool(vm, 3);

    if (width != 1 && width != 2 && width != 4)
    {
        be_raise(vm, FL_ERR, "width is 1, 2 or 4");
    }

    be_pushint(vm, (bint)se_word_sum(d, n, (uint8_t)width, big));
    be_return(vm);
}

int se_fb_sha256(bvm *vm)
{
    size_t n = 0;
    const uint8_t *d = se_fl_bytes(vm, 1, &n);
    uint8_t digest[32];
    size_t dn = 0;
    char hex[65];

    (void)psa_crypto_init();

    if (psa_hash_compute(PSA_ALG_SHA_256, d, n, digest, sizeof(digest), &dn) !=
        PSA_SUCCESS)
    {
        be_raise(vm, FL_ERR, "sha256 failed");
    }

    for (int i = 0; i < 32; i++)
    {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }

    be_pushstring(vm, hex);
    be_return(vm);
}
