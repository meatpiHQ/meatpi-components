#include "ml_routes.h"

/* WiCAN (2026-11-09): the tunnel's own network is a route like the others.
 *
 * Until then only the subnet routes went into ip4_routes and a peer's OWN
 * address (100.x) was left to lwIP's netif walk, which takes a netif only
 * while its link is up. The driver lowers the tunnel's link while no peer
 * holds a session, so the first packet for a peer's own address left on the
 * station in clear text, never reached the driver and never raised the
 * session want: a push to a peer that had not contacted the device first
 * timed out for ever while the VPN said connected (reproduced on beta.8
 * with a webhook at a peer's tailnet address, while chasing a field
 * report). ip4_routes needs the netif administratively up only, the rule
 * the subnet routes already depend on, so with the own network in the
 * table that first packet reaches the driver and starts the session. */
int ml_routes_build(microlink_t *ml, uint32_t own_addr, uint32_t own_mask,
                    ip4_routes_entry_t *out, int max, bool *dropped)
{
    int total = 0;
    int subnets = 0;

    *dropped = false;

    if (own_mask != 0 && total < max)
    {
        out[total].network = own_addr & own_mask;
        out[total].mask = own_mask;
        total++;
    }

    for (int i = 0; i < ml->peer_count; i++)
    {
        const ml_peer_t *p = &ml->peers[i];

        if (!p->active)
        {
            continue;
        }

        for (int r = 0; r < p->route_count; r++)
        {
            if (subnets >= ML_MAX_ROUTES || total >= max)
            {
                *dropped = true;
                break;
            }

            out[total].network = p->routes[r].network;
            out[total].mask = p->routes[r].mask;
            total++;

            ml->routes[subnets].network = p->routes[r].network;
            ml->routes[subnets].mask = p->routes[r].mask;
            ml->routes[subnets].via_vpn_ip = p->vpn_ip;
            subnets++;
        }
    }

    ml->route_count = subnets;
    return total;
}
