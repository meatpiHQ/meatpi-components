/* ml_routes.c: the tunnel's lwIP destination table (2026-11-09). The rule
 * under test: the tunnel's own network is always in the table, peers or no
 * peers, sessions or no sessions, so a packet for a peer's own address
 * reaches the driver and can ask for the session. */
#include "ml_routes.h"

#include <string.h>

#include "unity.h"

#define IP(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define TAILNET_NET IP(100, 64, 0, 0)
#define TAILNET_MASK IP(255, 192, 0, 0)
#define MASK24 IP(255, 255, 255, 0)

static microlink_t s_ml;
static ip4_routes_entry_t s_out[ML_ROUTES_TABLE_MAX];
static bool s_dropped;

static void reset(void)
{
    memset(&s_ml, 0, sizeof(s_ml));
    memset(s_out, 0xee, sizeof(s_out));
    s_ml.route_count = 99; /* must be rewritten */
    s_dropped = true;      /* must be rewritten */
}

static ml_peer_t *add_peer(uint32_t vpn_ip, int routes, uint32_t first_net)
{
    ml_peer_t *p = &s_ml.peers[s_ml.peer_count++];

    p->active = true;
    p->vpn_ip = vpn_ip;
    p->route_count = routes;

    for (int r = 0; r < routes; r++)
    {
        p->routes[r].network = first_net + ((uint32_t)r << 8);
        p->routes[r].mask = MASK24;
    }

    return p;
}

static int build(uint32_t own_addr, uint32_t own_mask, int max)
{
    return ml_routes_build(&s_ml, own_addr, own_mask, s_out, max, &s_dropped);
}

static void test_routes_own_network_without_any_peer(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(1, build(IP(100, 64, 0, 1), TAILNET_MASK, ML_ROUTES_TABLE_MAX));
    TEST_ASSERT_EQUAL_HEX32(TAILNET_NET, s_out[0].network);
    TEST_ASSERT_EQUAL_HEX32(TAILNET_MASK, s_out[0].mask);
    TEST_ASSERT_EQUAL_INT(0, s_ml.route_count);
    TEST_ASSERT_FALSE(s_dropped);
}

static void test_routes_own_network_is_the_address_masked(void)
{
    /* the reporter's tailnet: any address of the range gives the range */
    reset();
    add_peer(IP(100, 109, 224, 96), 0, 0);
    TEST_ASSERT_EQUAL_INT(1, build(IP(100, 92, 36, 66), TAILNET_MASK, ML_ROUTES_TABLE_MAX));
    TEST_ASSERT_EQUAL_HEX32(TAILNET_NET, s_out[0].network);
    TEST_ASSERT_EQUAL_HEX32(TAILNET_MASK, s_out[0].mask);
}

static void test_routes_subnets_follow_the_own_network(void)
{
    reset();
    add_peer(IP(100, 64, 0, 2), 1, IP(10, 99, 0, 0));
    add_peer(IP(100, 64, 0, 3), 2, IP(192, 168, 1, 0));
    TEST_ASSERT_EQUAL_INT(4, build(IP(100, 64, 0, 1), TAILNET_MASK, ML_ROUTES_TABLE_MAX));
    TEST_ASSERT_EQUAL_HEX32(TAILNET_NET, s_out[0].network);
    TEST_ASSERT_EQUAL_HEX32(IP(10, 99, 0, 0), s_out[1].network);
    TEST_ASSERT_EQUAL_HEX32(MASK24, s_out[1].mask);
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 1, 0), s_out[2].network);
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 2, 0), s_out[3].network);
    TEST_ASSERT_FALSE(s_dropped);
}

static void test_routes_status_lists_the_subnets_only(void)
{
    /* /api/vpn `routes` and the vpn command: the subnet routers' networks
       with their owner, never the tailnet's own range */
    reset();
    add_peer(IP(100, 64, 0, 2), 1, IP(10, 99, 0, 0));
    add_peer(IP(100, 64, 0, 3), 1, IP(192, 168, 1, 0));
    build(IP(100, 64, 0, 1), TAILNET_MASK, ML_ROUTES_TABLE_MAX);
    TEST_ASSERT_EQUAL_INT(2, s_ml.route_count);
    TEST_ASSERT_EQUAL_HEX32(IP(10, 99, 0, 0), s_ml.routes[0].network);
    TEST_ASSERT_EQUAL_HEX32(MASK24, s_ml.routes[0].mask);
    TEST_ASSERT_EQUAL_HEX32(IP(100, 64, 0, 2), s_ml.routes[0].via_vpn_ip);
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 1, 0), s_ml.routes[1].network);
    TEST_ASSERT_EQUAL_HEX32(IP(100, 64, 0, 3), s_ml.routes[1].via_vpn_ip);
}

static void test_routes_inactive_peer_is_skipped(void)
{
    reset();
    add_peer(IP(100, 64, 0, 2), 1, IP(10, 99, 0, 0))->active = false;
    add_peer(IP(100, 64, 0, 3), 1, IP(192, 168, 1, 0));
    TEST_ASSERT_EQUAL_INT(2, build(IP(100, 64, 0, 1), TAILNET_MASK, ML_ROUTES_TABLE_MAX));
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 1, 0), s_out[1].network);
    TEST_ASSERT_EQUAL_INT(1, s_ml.route_count);
    TEST_ASSERT_EQUAL_HEX32(IP(100, 64, 0, 3), s_ml.routes[0].via_vpn_ip);
}

static void test_routes_too_many_subnets_keep_the_own_network(void)
{
    reset();
    add_peer(IP(100, 64, 0, 2), 4, IP(10, 1, 0, 0));
    add_peer(IP(100, 64, 0, 3), 4, IP(10, 2, 0, 0));
    add_peer(IP(100, 64, 0, 4), 4, IP(10, 3, 0, 0));
    TEST_ASSERT_EQUAL_INT(ML_MAX_ROUTES + 1,
                          build(IP(100, 64, 0, 1), TAILNET_MASK, ML_ROUTES_TABLE_MAX));
    TEST_ASSERT_EQUAL_HEX32(TAILNET_NET, s_out[0].network);
    TEST_ASSERT_EQUAL_HEX32(IP(10, 2, 3, 0), s_out[ML_MAX_ROUTES].network);
    TEST_ASSERT_EQUAL_INT(ML_MAX_ROUTES, s_ml.route_count);
    TEST_ASSERT_TRUE(s_dropped);
}

static void test_routes_no_netmask_gives_no_own_network(void)
{
    reset();
    add_peer(IP(100, 64, 0, 2), 1, IP(10, 99, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, build(0, 0, ML_ROUTES_TABLE_MAX));
    TEST_ASSERT_EQUAL_HEX32(IP(10, 99, 0, 0), s_out[0].network);
    TEST_ASSERT_EQUAL_INT(1, s_ml.route_count);
    TEST_ASSERT_FALSE(s_dropped);
}

static void test_routes_never_write_past_the_buffer(void)
{
    reset();
    add_peer(IP(100, 64, 0, 2), 2, IP(10, 99, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, build(IP(100, 64, 0, 1), TAILNET_MASK, 1));
    TEST_ASSERT_EQUAL_HEX32(TAILNET_NET, s_out[0].network);
    TEST_ASSERT_EQUAL_HEX32(0xeeeeeeee, s_out[1].network);
    TEST_ASSERT_EQUAL_INT(0, s_ml.route_count);
    TEST_ASSERT_TRUE(s_dropped);
}

void run_routes_tests(void)
{
    RUN_TEST(test_routes_own_network_without_any_peer);
    RUN_TEST(test_routes_own_network_is_the_address_masked);
    RUN_TEST(test_routes_subnets_follow_the_own_network);
    RUN_TEST(test_routes_status_lists_the_subnets_only);
    RUN_TEST(test_routes_inactive_peer_is_skipped);
    RUN_TEST(test_routes_too_many_subnets_keep_the_own_network);
    RUN_TEST(test_routes_no_netmask_gives_no_own_network);
    RUN_TEST(test_routes_never_write_past_the_buffer);
}
