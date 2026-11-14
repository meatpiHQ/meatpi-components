/**
 * @file test_main.c
 * @brief Host tests for http_client_manager's pure helpers: URL
 *        validation, base64 (RFC 4648 vectors), Bearer/Basic building,
 *        header splitting, the redirect rule and the class of an address a
 *        request went to. Expected: 7 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "http_client_manager_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_url_validation(void)
{
    TEST_ASSERT_TRUE(hc_url_valid("http://10.42.0.1:8080/x"));
    TEST_ASSERT_TRUE(hc_url_valid("https://api.example.com/v1"));
    TEST_ASSERT_FALSE(hc_url_valid("ftp://x"));
    TEST_ASSERT_FALSE(hc_url_valid("http://"));
    TEST_ASSERT_FALSE(hc_url_valid(NULL));
    TEST_ASSERT_TRUE(hc_url_is_tls("https://x.y"));
    TEST_ASSERT_FALSE(hc_url_is_tls("http://x.y"));
}

static void test_base64_rfc_vectors(void)
{
    char out[32];

    TEST_ASSERT_EQUAL_size_t(4,
        hc_base64((const uint8_t *)"f", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Zg==", out);
    hc_base64((const uint8_t *)"fo", 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zm8=", out);
    hc_base64((const uint8_t *)"foo", 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zm9v", out);
    hc_base64((const uint8_t *)"foobar", 6, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", out);
    /* capacity gate */
    TEST_ASSERT_EQUAL_size_t(0,
        hc_base64((const uint8_t *)"foobar", 6, out, 8));
}

static void test_bearer(void)
{
    char out[64];

    TEST_ASSERT_TRUE(hc_auth_bearer("tok123", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Bearer tok123", out);
    TEST_ASSERT_FALSE(hc_auth_bearer("", out, sizeof(out)));
    TEST_ASSERT_FALSE(hc_auth_bearer("way-too-long", out, 8));
}

static void test_basic_rfc7617_vector(void)
{
    char out[64];

    TEST_ASSERT_TRUE(hc_auth_basic("Aladdin", "open sesame", out,
                                   sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==", out);
    TEST_ASSERT_FALSE(hc_auth_basic(NULL, "x", out, sizeof(out)));
}

static void test_header_split(void)
{
    char key[64];
    const char *val;

    val = hc_header_split("X-Custom: hello world", key, sizeof(key));
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("X-Custom", key);
    TEST_ASSERT_EQUAL_STRING("hello world", val);
    TEST_ASSERT_NULL(hc_header_split("no-colon-here", key, sizeof(key)));
    TEST_ASSERT_NULL(hc_header_split(": empty-key", key, sizeof(key)));
    TEST_ASSERT_NULL(hc_header_split("Key:", key, sizeof(key)));
}

static void test_redirect_rule(void)
{
    /* the GitHub release asset: 302 to the signed CDN address */
    TEST_ASSERT_TRUE(hc_redirect_follow(302, true, 0, HC_MAX_REDIRECTS));
    TEST_ASSERT_TRUE(hc_redirect_follow(301, true, 0, HC_MAX_REDIRECTS));
    TEST_ASSERT_TRUE(hc_redirect_follow(303, true, 1, HC_MAX_REDIRECTS));
    TEST_ASSERT_TRUE(hc_redirect_follow(307, true, 2, HC_MAX_REDIRECTS));
    TEST_ASSERT_TRUE(hc_redirect_follow(308, true, 2, HC_MAX_REDIRECTS));
    /* three hops, then the 3xx is the answer */
    TEST_ASSERT_FALSE(hc_redirect_follow(302, true, 3, HC_MAX_REDIRECTS));
    /* not a redirect */
    TEST_ASSERT_FALSE(hc_redirect_follow(200, true, 0, HC_MAX_REDIRECTS));
    TEST_ASSERT_FALSE(hc_redirect_follow(304, true, 0, HC_MAX_REDIRECTS));
    TEST_ASSERT_FALSE(hc_redirect_follow(404, true, 0, HC_MAX_REDIRECTS));
    /* a POST keeps its 3xx */
    TEST_ASSERT_FALSE(hc_redirect_follow(302, false, 0, HC_MAX_REDIRECTS));
}

static bool ip4_private(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t addr[4] = { a, b, c, d };

    return hc_ip4_private(addr);
}

/* where a request went (2026-11-14): ha_webhooks takes settings from a
   push's reply over plain http only from an address that is not routed on
   the internet */
static void test_private_addresses(void)
{
    /* the home LAN ranges, edge to edge */
    TEST_ASSERT_TRUE(ip4_private(10, 0, 0, 1));
    TEST_ASSERT_TRUE(ip4_private(10, 255, 255, 254));
    TEST_ASSERT_TRUE(ip4_private(172, 16, 0, 1));
    TEST_ASSERT_TRUE(ip4_private(172, 31, 255, 254));
    TEST_ASSERT_FALSE(ip4_private(172, 15, 0, 1));
    TEST_ASSERT_FALSE(ip4_private(172, 32, 0, 1));
    TEST_ASSERT_TRUE(ip4_private(192, 168, 1, 10));
    TEST_ASSERT_FALSE(ip4_private(192, 169, 1, 10));
    TEST_ASSERT_FALSE(ip4_private(193, 168, 1, 10));
    /* carrier NAT and the VPN tunnels' range, 100.64/10 */
    TEST_ASSERT_TRUE(ip4_private(100, 64, 0, 1));
    TEST_ASSERT_TRUE(ip4_private(100, 127, 255, 254));
    TEST_ASSERT_FALSE(ip4_private(100, 63, 0, 1));
    TEST_ASSERT_FALSE(ip4_private(100, 128, 0, 1));
    /* link-local */
    TEST_ASSERT_TRUE(ip4_private(169, 254, 10, 10));
    /* the internet, the documentation ranges, loopback, nothing */
    TEST_ASSERT_FALSE(ip4_private(8, 8, 8, 8));
    TEST_ASSERT_FALSE(ip4_private(203, 0, 113, 1));
    TEST_ASSERT_FALSE(ip4_private(127, 0, 0, 1));
    TEST_ASSERT_FALSE(ip4_private(0, 0, 0, 0));

    /* IPv6: unique-local and link-local, an IPv4 peer on a v6 socket by
       the IPv4 rule, a global address not */
    const uint8_t ula[16] = { 0xfd, 0x12, 0x34 };
    const uint8_t ula_c[16] = { 0xfc, 0x00 };
    const uint8_t ll[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1 };
    const uint8_t site[16] = { 0xfe, 0xc0 };
    const uint8_t global[16] = { 0x20, 0x01, 0x0d, 0xb8 };
    const uint8_t mapped_lan[16] =
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 1, 10 };
    const uint8_t mapped_pub[16] =
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 8, 8, 8, 8 };
    const uint8_t loop[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

    TEST_ASSERT_TRUE(hc_ip6_private(ula));
    TEST_ASSERT_TRUE(hc_ip6_private(ula_c));
    TEST_ASSERT_TRUE(hc_ip6_private(ll));
    TEST_ASSERT_FALSE(hc_ip6_private(site));
    TEST_ASSERT_FALSE(hc_ip6_private(global));
    TEST_ASSERT_TRUE(hc_ip6_private(mapped_lan));
    TEST_ASSERT_FALSE(hc_ip6_private(mapped_pub));
    TEST_ASSERT_FALSE(hc_ip6_private(loop));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_url_validation);
    RUN_TEST(test_base64_rfc_vectors);
    RUN_TEST(test_bearer);
    RUN_TEST(test_basic_rfc7617_vector);
    RUN_TEST(test_header_split);
    RUN_TEST(test_redirect_rule);
    RUN_TEST(test_private_addresses);
    UNITY_END();
}
