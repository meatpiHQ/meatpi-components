/**
 * @file test_pack.c
 * @brief Host tests for ble_manager's pure layer: the legacy TX packing
 *        math (round-up packets, fill/flush across chunk boundaries) and
 *        the identity derivations existing tools depend on.
 */
#include <string.h>

#include "unity.h"

#include "ble_manager_private.h"

/* ---- packets needed (legacy round-up) ------------------------------------------ */

static void test_packets_needed_rounds_up(void)
{
    TEST_ASSERT_EQUAL(1, blm_pack_packets_needed(0, 20, 20));
    TEST_ASSERT_EQUAL(2, blm_pack_packets_needed(0, 21, 20));
    TEST_ASSERT_EQUAL(1, blm_pack_packets_needed(5, 10, 20));
    TEST_ASSERT_EQUAL(2, blm_pack_packets_needed(5, 16, 20));  /* 21 total */
    TEST_ASSERT_EQUAL(7, blm_pack_packets_needed(0, 128, 20)); /* 6.4 -> 7 */
    TEST_ASSERT_EQUAL(0, blm_pack_packets_needed(0, 10, 0));   /* guard    */
}

/* ---- fill/flush state machine ----------------------------------------------------- */

static void test_fill_partial_then_full(void)
{
    uint8_t buf[20];
    size_t buf_len = 0;
    size_t consumed = 0;
    const uint8_t in[15] = "0123456789abcd";

    /* 15 bytes into an empty 20-byte packet: not full yet */
    TEST_ASSERT_FALSE(blm_pack_fill(buf, &buf_len, 20, in, 15, &consumed));
    TEST_ASSERT_EQUAL(15, buf_len);
    TEST_ASSERT_EQUAL(15, consumed);

    /* 15 more from a second chunk: fills at 20, leaves 10 unconsumed */
    consumed = 0;
    TEST_ASSERT_TRUE(blm_pack_fill(buf, &buf_len, 20, in, 15, &consumed));
    TEST_ASSERT_EQUAL(20, buf_len);
    TEST_ASSERT_EQUAL(5, consumed);

    /* the flush + remainder: bytes in[5..] continue in order */
    buf_len = 0;
    TEST_ASSERT_FALSE(blm_pack_fill(buf, &buf_len, 20, in, 15, &consumed));
    TEST_ASSERT_EQUAL(10, buf_len);
    TEST_ASSERT_EQUAL(15, consumed);
    TEST_ASSERT_EQUAL_MEMORY("56789abcd", buf, 9); /* order preserved */
}

static void test_fill_exact_boundary(void)
{
    uint8_t buf[20];
    size_t buf_len = 0;
    size_t consumed = 0;
    uint8_t in[20];

    memset(in, 0xAB, sizeof(in));
    TEST_ASSERT_TRUE(blm_pack_fill(buf, &buf_len, 20, in, 20, &consumed));
    TEST_ASSERT_EQUAL(20, consumed); /* nothing left over */
}

/* ---- identity (the on-air values existing tools read) ------------------------------ */

static void test_name_from_id(void)
{
    char name[BLM_NAME_MAX];

    /* legacy ble_uid format: "WiC_" + full 12-hex device id */
    blm_ident_name("aabbccddeeff", name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("WiC_aabbccddeeff", name);
}

static void test_serial_legacy_plus7(void)
{
    char serial[32];

    /* legacy verbatim: serial = dev_name + 7 -> last 9 hex chars */
    blm_ident_serial("WiC_aabbccddeeff", serial, sizeof(serial));
    TEST_ASSERT_EQUAL_STRING("bccddeeff", serial);

    blm_ident_serial("short", serial, sizeof(serial));
    TEST_ASSERT_EQUAL_STRING("", serial); /* too short: empty, no overrun */
}

static void test_tx_power_clamp(void)
{
    TEST_ASSERT_EQUAL(9, blm_ident_clamp_tx_power(9));
    TEST_ASSERT_EQUAL(9, blm_ident_clamp_tx_power(20));   /* above range  */
    TEST_ASSERT_EQUAL(-12, blm_ident_clamp_tx_power(-40)); /* below range */
    TEST_ASSERT_EQUAL(0, blm_ident_clamp_tx_power(1));    /* nearest step */
    TEST_ASSERT_EQUAL(3, blm_ident_clamp_tx_power(2));
    TEST_ASSERT_EQUAL(-6, blm_ident_clamp_tx_power(-5));
}

static void test_conn_profile_mapping(void)
{
    uint16_t mn = 0;
    uint16_t mx = 0;

    blm_ident_conn_window("ios", &mn, &mx);
    TEST_ASSERT_EQUAL_HEX16(0x10, mn); /* legacy 20 ms */
    TEST_ASSERT_EQUAL_HEX16(0x20, mx); /* legacy 40 ms */

    blm_ident_conn_window("android_fast", &mn, &mx);
    TEST_ASSERT_EQUAL_HEX16(0x06, mn); /* 7.5 ms */
    TEST_ASSERT_EQUAL_HEX16(0x0C, mx); /* 15 ms  */

    /* unknown/missing falls back to the iOS-friendly default */
    blm_ident_conn_window(NULL, &mn, &mx);
    TEST_ASSERT_EQUAL_HEX16(0x10, mn);
    blm_ident_conn_window("warp_speed", &mn, &mx);
    TEST_ASSERT_EQUAL_HEX16(0x10, mn);
}

void run_pack_tests(void)
{
    RUN_TEST(test_packets_needed_rounds_up);
    RUN_TEST(test_fill_partial_then_full);
    RUN_TEST(test_fill_exact_boundary);
    RUN_TEST(test_name_from_id);
    RUN_TEST(test_serial_legacy_plus7);
    RUN_TEST(test_tx_power_clamp);
    RUN_TEST(test_conn_profile_mapping);
}
