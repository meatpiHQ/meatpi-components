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
 * @file ble_central_bench_disc.c
 * @brief GATT discovery on the secured link: the FFF0 service, its
 *        characteristics (FFF1..FFF4 value handles), the CCCDs of the two
 *        notify characteristics, and the Device Information manufacturer
 *        string (2A29). A chain of NimBLE discovery callbacks (host task)
 *        that ends in bcb_core_on_discovered().
 */
#include <string.h>

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include "ble_central_bench_private.h"

static uint16_t s_conn;
static bcb_chars_t s_chars;
static uint16_t s_svc_start, s_svc_end;
static struct { uint16_t def, val; uint16_t uuid; } s_chr[8];
static int      s_nchr, s_dsc_idx;

static const ble_uuid16_t UUID_FFF0 = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t UUID_2A29 = BLE_UUID16_INIT(0x2A29);

/* ---- discovery --------------------------------------------------------------------- */

static void discovery_done(bool ok)
{
    bcb_diag("discovery %s: fff1 %u (cccd %u) fff2 %u fff3 %u (cccd %u) fff4 %u dis %u",
             ok ? "done" : "FAILED", s_chars.fff1, s_chars.fff1_cccd, s_chars.fff2,
             s_chars.fff3, s_chars.fff3_cccd, s_chars.fff4, s_chars.dis_mfr);
    bcb_core_on_discovered(&s_chars, ok);
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val,
                  const struct ble_gatt_dsc *dsc, void *arg);
static int dis_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg);

/* descriptors of the notify characteristics, one at a time (FFF1 then FFF3) */
static void disc_next_dscs(void)
{
    while (s_dsc_idx < s_nchr)
    {
        int i = s_dsc_idx++;

        if (s_chr[i].uuid == 0xFFF1 || s_chr[i].uuid == 0xFFF3)
        {
            uint16_t end = (i + 1 < s_nchr) ? s_chr[i + 1].def - 1 : s_svc_end;
            int rc = ble_gattc_disc_all_dscs(s_conn, s_chr[i].val, end, dsc_cb, NULL);

            if (rc == 0)
            {
                return;
            }

            bcb_diag("descriptor discovery failed (rc %d)", rc);
        }
    }

    /* then the Device Information manufacturer string */
    if (ble_gattc_disc_chrs_by_uuid(s_conn, 1, 0xFFFF, &UUID_2A29.u, dis_cb, NULL) != 0)
    {
        discovery_done(s_chars.fff1 && s_chars.fff2);
    }
}

static int dis_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;

    if (err->status == 0 && chr != NULL)
    {
        s_chars.dis_mfr = chr->val_handle;
        return 0;
    }

    discovery_done(s_chars.fff1 && s_chars.fff2);
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)arg;

    if (err->status == 0 && dsc != NULL)
    {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902)
        {
            if (chr_val == s_chars.fff1) s_chars.fff1_cccd = dsc->handle;
            if (chr_val == s_chars.fff3) s_chars.fff3_cccd = dsc->handle;
        }

        return 0;
    }

    disc_next_dscs(); /* BLE_HS_EDONE or an error: move on */
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;

    if (err->status == 0 && chr != NULL)
    {
        if (s_nchr < (int)(sizeof(s_chr) / sizeof(s_chr[0])))
        {
            uint16_t u = ble_uuid_u16(&chr->uuid.u);

            s_chr[s_nchr].def = chr->def_handle;
            s_chr[s_nchr].val = chr->val_handle;
            s_chr[s_nchr].uuid = u;
            s_nchr++;

            switch (u)
            {
                case 0xFFF1: s_chars.fff1 = chr->val_handle; break;
                case 0xFFF2: s_chars.fff2 = chr->val_handle; break;
                case 0xFFF3: s_chars.fff3 = chr->val_handle; break;
                case 0xFFF4: s_chars.fff4 = chr->val_handle; break;
                default: break;
            }
        }

        return 0;
    }

    s_dsc_idx = 0;
    disc_next_dscs();
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;

    if (err->status == 0 && svc != NULL)
    {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }

    if (s_svc_start == 0)
    {
        bcb_diag("FFF0 service not found");
        discovery_done(false);
        return 0;
    }

    s_nchr = 0;

    if (ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, chr_cb, NULL) != 0)
    {
        discovery_done(false);
    }

    return 0;
}

void bcb_disc_start(uint16_t conn)
{
    s_conn = conn;
    memset(&s_chars, 0, sizeof(s_chars));
    s_svc_start = s_svc_end = 0;

    if (ble_gattc_disc_svc_by_uuid(s_conn, &UUID_FFF0.u, svc_cb, NULL) != 0)
    {
        discovery_done(false);
    }
}

