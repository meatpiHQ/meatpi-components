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
 * @file external_storage_private.h
 * @brief Internal API + the pure detect-debounce state machine
 *        (host-testable — no GPIO).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* consecutive identical samples required to accept a state change —
 * SD sockets bounce on insertion, and 4 × 250 ms also rides out the
 * card seating wobble */
#define ES_DEBOUNCE_SAMPLES 4

typedef struct
{
    bool stable_present;   /* the debounced truth                       */
    bool candidate;        /* level being counted toward a change       */
    int  count;            /* consecutive samples of `candidate`        */
} es_detect_t;

/** Start with an assumed state (first real sample overrides quickly). */
void es_detect_init(es_detect_t *d, bool initial_present);

/**
 * Feed one raw sample; returns true when the DEBOUNCED state just changed
 * (read the new state from d->stable_present). Glitches shorter than
 * ES_DEBOUNCE_SAMPLES never surface.
 */
bool es_detect_feed(es_detect_t *d, bool raw_present);

/** Mounted card's identity for the `sdcard` CLI (-i/--info). */
typedef struct
{
    char        name[8];        /* CID product name                    */
    const char *type;           /* "SDHC/SDXC" / "SDSC" / "MMC" / "SDIO" */
    uint64_t    capacity_bytes;
    int         sector_size;
    uint32_t    speed_khz;
} es_card_details_t;

/** Fill @p out from the mounted card. False when no card is mounted. */
bool es_card_details(es_card_details_t *out);

#ifdef __cplusplus
}
#endif
