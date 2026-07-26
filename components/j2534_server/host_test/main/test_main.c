/**
 * @file test_main.c
 * @brief Host tests for j2534_proto: header codec round-trip, bad-magic /
 *        short-buffer rejection, PASSTHRU_MSG round-trip + bounds, and
 *        the J2534 filter-match semantics.
 *        Expected output: 8 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "j2534_proto.h"

void setUp(void) {}
void tearDown(void) {}

static void test_hdr_roundtrip(void)
{
    uint8_t buf[J2534_HDR_SIZE];
    TEST_ASSERT_EQUAL(J2534_HDR_SIZE,
        j2534_hdr_encode(buf, J2534_MT_CONNECT, 0x1234, 7, 12));

    j2534_hdr_t h;
    TEST_ASSERT_TRUE(j2534_hdr_decode(buf, sizeof(buf), &h));
    TEST_ASSERT_EQUAL_HEX16(J2534_MAGIC, h.magic);
    TEST_ASSERT_EQUAL(J2534_WIRE_VERSION, h.version);
    TEST_ASSERT_EQUAL(J2534_MT_CONNECT, h.type);
    TEST_ASSERT_EQUAL_HEX16(0x1234, h.seq);
    TEST_ASSERT_EQUAL(7, h.channel);
    TEST_ASSERT_EQUAL(12, h.length);
}

static void test_hdr_rejects(void)
{
    uint8_t buf[J2534_HDR_SIZE];
    j2534_hdr_encode(buf, J2534_MT_HELLO, 1, 0, 0);
    j2534_hdr_t h;

    /* short buffer */
    TEST_ASSERT_FALSE(j2534_hdr_decode(buf, J2534_HDR_SIZE - 1, &h));
    /* bad magic */
    buf[0] ^= 0xFF;
    TEST_ASSERT_FALSE(j2534_hdr_decode(buf, sizeof(buf), &h));
    /* bad version */
    j2534_hdr_encode(buf, J2534_MT_HELLO, 1, 0, 0);
    buf[2] = 99;
    TEST_ASSERT_FALSE(j2534_hdr_decode(buf, sizeof(buf), &h));
}

static void test_msg_roundtrip(void)
{
    j2534_msg_t m = { 0 };
    m.protocol_id = J2534_PROT_ISO15765;
    m.tx_flags = J2534_TX_ISO15765_FRAME_PAD;
    m.timestamp = 0xDEADBEEF;
    m.data_size = 5;
    memcpy(m.data, (uint8_t[]){ 0x22, 0xF1, 0x90, 0xAA, 0xBB }, 5);

    uint8_t buf[64];
    size_t n = j2534_msg_encode(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(24 + 5, n);

    j2534_msg_t out;
    TEST_ASSERT_TRUE(j2534_msg_decode(buf, n, &out));
    TEST_ASSERT_EQUAL(J2534_PROT_ISO15765, out.protocol_id);
    TEST_ASSERT_EQUAL_HEX32(J2534_TX_ISO15765_FRAME_PAD, out.tx_flags);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, out.timestamp);
    TEST_ASSERT_EQUAL(5, out.data_size);
    TEST_ASSERT_EQUAL_HEX8(0x22, out.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, out.data[4]);
}

static void test_msg_bounds(void)
{
    j2534_msg_t m = { 0 };
    m.data_size = 10;
    uint8_t small[20];
    /* buffer too small for fixed header + data */
    TEST_ASSERT_EQUAL(0, j2534_msg_encode(&m, small, sizeof(small)));

    /* decode with a truncated data region must fail */
    uint8_t buf[64];
    m.data_size = 5;
    size_t n = j2534_msg_encode(&m, buf, sizeof(buf));
    j2534_msg_t out;
    TEST_ASSERT_FALSE(j2534_msg_decode(buf, n - 1, &out));

    /* oversized data_size claim rejected */
    uint8_t fake[24];
    memset(fake, 0, sizeof(fake));
    fake[20] = 0xFF; fake[21] = 0xFF; /* huge data_size */
    TEST_ASSERT_FALSE(j2534_msg_decode(fake, sizeof(fake), &out));
}

static void test_filter_pass_all(void)
{
    const uint8_t msg[] = { 0x07, 0xE8, 0x62 };
    /* zero-length mask = PASS everything */
    TEST_ASSERT_TRUE(j2534_filter_match(NULL, 0, NULL, 0,
                                        msg, sizeof(msg)));
}

static void test_filter_exact_id(void)
{
    /* match a 4-byte CAN id 0x000007E8 */
    const uint8_t mask[]    = { 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t pattern[] = { 0x00, 0x00, 0x07, 0xE8 };
    const uint8_t yes[]     = { 0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1 };
    const uint8_t no[]      = { 0x00, 0x00, 0x07, 0xDF, 0x03 };

    TEST_ASSERT_TRUE(j2534_filter_match(mask, 4, pattern, 4,
                                        yes, sizeof(yes)));
    TEST_ASSERT_FALSE(j2534_filter_match(mask, 4, pattern, 4,
                                         no, sizeof(no)));
}

static void test_filter_partial_mask(void)
{
    /* care only about the high nibble of byte 0 */
    const uint8_t mask[]    = { 0xF0 };
    const uint8_t pattern[] = { 0x70 };
    const uint8_t yes[]     = { 0x7A, 0xBC };
    const uint8_t no[]      = { 0x8A };

    TEST_ASSERT_TRUE(j2534_filter_match(mask, 1, pattern, 1,
                                        yes, sizeof(yes)));
    TEST_ASSERT_FALSE(j2534_filter_match(mask, 1, pattern, 1,
                                         no, sizeof(no)));
}

static void test_status_names(void)
{
    TEST_ASSERT_EQUAL_STRING("NOERROR",
        j2534_status_name(J2534_STATUS_NOERROR));
    TEST_ASSERT_EQUAL_STRING("ERR_NOT_SUPPORTED",
        j2534_status_name(J2534_ERR_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_STRING("ERR_DEVICE_IN_USE",
        j2534_status_name(J2534_ERR_DEVICE_IN_USE));
    TEST_ASSERT_EQUAL_STRING("?", j2534_status_name(0xABCD));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hdr_roundtrip);
    RUN_TEST(test_hdr_rejects);
    RUN_TEST(test_msg_roundtrip);
    RUN_TEST(test_msg_bounds);
    RUN_TEST(test_filter_pass_all);
    RUN_TEST(test_filter_exact_id);
    RUN_TEST(test_filter_partial_mask);
    RUN_TEST(test_status_names);
    UNITY_END();
}
