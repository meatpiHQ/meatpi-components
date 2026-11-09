#pragma once

#include "microlink_internal.h"
#include "ip4_routes.h"

/* Private to microlink (WiCAN, 2026-11-09). Pure: no lwIP, host-tested.
 *
 * The tunnel's lwIP destination table: the tunnel's own network first
 * (own_addr & own_mask, host order; left out when own_mask is 0), then the
 * subnet routes of every active peer (at most ML_MAX_ROUTES). Writes at
 * most max entries to out and returns their count; *dropped says a subnet
 * route did not fit. ml->routes / ml->route_count, the status surface, take
 * the subnet routes only. */
#define ML_ROUTES_TABLE_MAX (ML_MAX_ROUTES + 1)

int ml_routes_build(microlink_t *ml, uint32_t own_addr, uint32_t own_mask,
                    ip4_routes_entry_t *out, int max, bool *dropped);
