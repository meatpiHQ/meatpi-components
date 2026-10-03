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
 * @file j1939_claim_core.c
 * @brief The NAME and the address claim (J1939-81). Pure: no IDF, no clock.
 */
#include "j1939_claim_core.h"

#include <string.h>

/* ---- NAME --------------------------------------------------------------------- */

void j1939_name_build(uint32_t identity, uint16_t manufacturer,
                      uint8_t function, bool arbitrary,
                      uint8_t out[J1939_NAME_LEN])
{
    uint64_t n = (uint64_t)(identity & 0x1FFFFFu) |
                 ((uint64_t)(manufacturer & 0x7FFu) << 21) |
                 ((uint64_t)function << 40) |
                 ((uint64_t)(arbitrary ? 1u : 0u) << 63);

    for (size_t i = 0; i < J1939_NAME_LEN; i++)
    {
        out[i] = (uint8_t)(n >> (8 * i));
    }
}

int j1939_name_compare(const uint8_t a[J1939_NAME_LEN],
                       const uint8_t b[J1939_NAME_LEN])
{
    /* an unsigned number, least significant byte first on the wire */
    for (size_t i = J1939_NAME_LEN; i > 0; i--)
    {
        if (a[i - 1] != b[i - 1])
        {
            return (a[i - 1] < b[i - 1]) ? -1 : 1;
        }
    }

    return 0;
}

/* ---- the claim ----------------------------------------------------------------- */

static void out_claim(const j1939_claim_t *c, uint8_t sa,
                      j1939_claim_out_t *out)
{
    if (out != NULL)
    {
        out->send = true;
        out->sa = sa;
        memcpy(out->data, c->name, J1939_NAME_LEN);
    }
}

static void out_none(j1939_claim_out_t *out)
{
    if (out != NULL)
    {
        out->send = false;
        out->sa = J1939_ADDR_NULL;
        memset(out->data, 0xFF, J1939_NAME_LEN);
    }
}

static void claim(j1939_claim_t *c, uint8_t sa, int64_t now_us,
                  j1939_claim_out_t *out)
{
    c->state = J1939_CLAIM_CLAIMING;
    c->sa = sa;
    c->sent_us = now_us;
    c->defended_us = 0; /* a new address: a new round of contests */
    c->stats.claims_sent++;
    out_claim(c, sa, out);
}

static void cannot(j1939_claim_t *c, j1939_claim_out_t *out)
{
    c->state = J1939_CLAIM_CANNOT;
    c->sa = J1939_ADDR_NULL;
    c->stats.cannot++;
    out_claim(c, J1939_ADDR_NULL, out);
}

/** The address to try after losing @p lost; J1939_ADDR_NULL when there is
 *  none left. The preferred one first, the other tool address, then the
 *  dynamic range once, upwards. */
static uint8_t next_candidate(j1939_claim_t *c, uint8_t lost)
{
    if (!c->tried_tool2 && c->preferred != J1939_CLAIM_TOOL_2 &&
        lost != J1939_CLAIM_TOOL_2)
    {
        c->tried_tool2 = true;
        return J1939_CLAIM_TOOL_2;
    }

    c->tried_tool2 = true;

    while (c->next_dyn <= J1939_CLAIM_ADDR_MAX)
    {
        uint8_t a = c->next_dyn++;

        if (a != c->preferred)
        {
            return a;
        }
    }

    return J1939_ADDR_NULL;
}

void j1939_claim_init(j1939_claim_t *c, const uint8_t name[J1939_NAME_LEN],
                      uint8_t preferred)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->name, name, J1939_NAME_LEN);
    c->preferred = preferred;
    c->sa = J1939_ADDR_NULL;
    c->next_dyn = J1939_CLAIM_ADDR_MIN;
}

void j1939_claim_start(j1939_claim_t *c, int64_t now_us,
                       j1939_claim_out_t *out)
{
    c->next_dyn = J1939_CLAIM_ADDR_MIN;
    c->tried_tool2 = false;
    claim(c, c->preferred, now_us, out);
}

void j1939_claim_tick(j1939_claim_t *c, int64_t now_us)
{
    if (c->state == J1939_CLAIM_CLAIMING &&
        now_us - c->sent_us >= J1939_CLAIM_WAIT_US)
    {
        c->state = J1939_CLAIM_CLAIMED;
    }
}

void j1939_claim_rx(j1939_claim_t *c, uint8_t sa,
                    const uint8_t name[J1939_NAME_LEN], int64_t now_us,
                    j1939_claim_out_t *out)
{
    out_none(out);

    if ((c->state != J1939_CLAIM_CLAIMING && c->state != J1939_CLAIM_CLAIMED) ||
        sa != c->sa || sa == J1939_ADDR_NULL)
    {
        return; /* somebody else's address, or a cannot-claim */
    }

    c->stats.contests++;

    if (j1939_name_compare(c->name, name) <= 0)
    {
        /* ours is the lower NAME (an equal one is a configuration error on
           the bus: defend anyway): say so again. The winner keeps using the
           address, there is no second wait (J1939-81 4.4.3.3). Once per
           round: the other node heard our claim already (the one it is
           contesting, or our last defence) */
        c->stats.won++;

        if (c->defended_us != 0 &&
            now_us - c->defended_us < J1939_CLAIM_DEFEND_HOLD_US)
        {
            c->stats.held++;
            return;
        }

        c->defended_us = now_us;
        c->stats.claims_sent++;
        out_claim(c, c->sa, out);
        return;
    }

    c->stats.lost++;

    uint8_t next = next_candidate(c, c->sa);

    if (next == J1939_ADDR_NULL)
    {
        cannot(c, out);
        return;
    }

    claim(c, next, now_us, out);
}

void j1939_claim_request(j1939_claim_t *c, int64_t now_us,
                         j1939_claim_out_t *out)
{
    (void)now_us; /* a request does not restart the wait */
    out_none(out);

    switch (c->state)
    {
    case J1939_CLAIM_CLAIMING:
    case J1939_CLAIM_CLAIMED:
        c->stats.requests++;
        c->stats.claims_sent++;
        out_claim(c, c->sa, out);
        return;

    case J1939_CLAIM_CANNOT:
        c->stats.requests++;
        c->stats.cannot++;
        out_claim(c, J1939_ADDR_NULL, out);
        return;

    default:
        return;
    }
}

bool j1939_claim_ready(const j1939_claim_t *c)
{
    return c->state == J1939_CLAIM_CLAIMED;
}

const char *j1939_claim_state_name(j1939_claim_state_t state)
{
    switch (state)
    {
    case J1939_CLAIM_CLAIMING: return "claiming";
    case J1939_CLAIM_CLAIMED:  return "claimed";
    case J1939_CLAIM_CANNOT:   return "cannot_claim";
    default:                   return "idle";
    }
}
