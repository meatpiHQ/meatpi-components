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
 * @file ble_manager_pack.c
 * @brief Pure helpers: the legacy TX packing math, identity derivations and
 *        TX-power clamping. No BT stack — host-tests on the linux target.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "ble_manager_private.h"

int blm_pack_packets_needed(size_t pending, size_t add, size_t max_data)
{
    if (max_data == 0)
    {
        return 0;
    }

    size_t total = pending + add;
    int packets = (int)(total / max_data);

    if ((total % max_data) != 0)
    {
        packets++; /* legacy round-up: a partial packet still costs one */
    }

    return packets;
}

bool blm_pack_fill(uint8_t *buf, size_t *buf_len, size_t max_data,
                   const uint8_t *in, size_t in_len, size_t *consumed)
{
    size_t buf_remaining = max_data - *buf_len;
    size_t in_remaining = in_len - *consumed;
    size_t copy = (in_remaining >= buf_remaining) ? buf_remaining
                                                  : in_remaining;

    memcpy(buf + *buf_len, in + *consumed, copy);
    *buf_len += copy;
    *consumed += copy;
    return *buf_len == max_data;
}

void blm_ident_name(const char *device_id, char *name, size_t name_len)
{
    /* legacy ble_uid format "WiC_<12-hex MAC>" (legacy main.c) — with the
       serial = name+7 rule below this also makes 2A25 byte-identical to
       legacy (last 9 hex chars). Was "WiCAN_<id>" until the 2026-07-18
       defaults pass restored legacy parity. */
    snprintf(name, name_len, "WiC_%s", device_id);
}

void blm_ident_serial(const char *dev_name, char *serial, size_t serial_len)
{
    /* legacy verbatim: serial = dev_name + 7 — this is what existing tools
       read from characteristic 2A25, so it stays byte-identical */
    size_t name_len = strlen(dev_name);

    serial[0] = '\0';

    if (name_len > 7 && name_len - 7 < serial_len)
    {
        strcpy(serial, dev_name + 7);
    }
}

void blm_ident_conn_window(const char *profile, uint16_t *min_units,
                           uint16_t *max_units)
{
    if (profile != NULL && strcmp(profile, "android_fast") == 0)
    {
        /* max performance: request 7.5–15 ms (units of 1.25 ms) */
        *min_units = 0x06;
        *max_units = 0x0C;
        return;
    }

    /* "ios" and anything unknown: the legacy iOS-friendly 20–40 ms */
    *min_units = 0x10;
    *max_units = 0x20;
}

uint8_t blm_ident_phy_mask(const char *phy)
{
    if (phy != NULL && strcmp(phy, "2m") == 0)
    {
        return BLM_PHY_2M;              /* strict: 2M, the LL keeps 1M if the peer cannot */
    }

    if (phy != NULL && strcmp(phy, "coded") == 0)
    {
        return BLM_PHY_CODED;           /* long range (S=2/S=8), same fallback rule */
    }

    if (phy != NULL && strcmp(phy, "auto") == 0)
    {
        return BLM_PHY_1M | BLM_PHY_2M; /* whichever the peer negotiates */
    }

    return BLM_PHY_1M;                  /* "1m" and anything unknown: 4.2 behaviour */
}

uint8_t blm_ident_adv_mode(const char *mode)
{
    if (mode != NULL && strcmp(mode, "extended") == 0)
    {
        return BLM_ADV_EXTENDED;
    }

    if (mode != NULL && strcmp(mode, "both") == 0)
    {
        return BLM_ADV_BOTH;
    }

    return BLM_ADV_LEGACY;              /* "legacy" and anything unknown */
}

int blm_ident_clamp_tx_power(int dbm)
{
    static const int LEVELS[] = { -12, -9, -6, -3, 0, 3, 6, 9 };
    int best = 9; /* legacy default: high power */
    int best_diff = 255;

    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++)
    {
        int diff = dbm - LEVELS[i];

        if (diff < 0)
        {
            diff = -diff;
        }

        if (diff < best_diff)
        {
            best_diff = diff;
            best = LEVELS[i];
        }
    }

    return best;
}

/* ---- stream channel OUT mode ------------------------------------------------------ */

uint8_t blm_channel_pick_out(uint8_t allowed, bool sub_notify, bool sub_indicate)
{
    if (allowed == 0)
    {
        allowed = BLE_MANAGER_CH_OUT_INDICATE; /* the pre-2026-09-22 default */
    }

    if (sub_notify && (allowed & BLE_MANAGER_CH_OUT_NOTIFY))
    {
        return BLE_MANAGER_CH_OUT_NOTIFY;
    }

    if (sub_indicate && (allowed & BLE_MANAGER_CH_OUT_INDICATE))
    {
        return BLE_MANAGER_CH_OUT_INDICATE;
    }

    /* nothing usable subscribed: indications, which NimBLE sends without
       a CCCD (custom indicate) - the behaviour every client saw so far */
    return BLE_MANAGER_CH_OUT_INDICATE;
}
