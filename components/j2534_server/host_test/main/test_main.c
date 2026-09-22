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

/* ---- the transport-agnostic framer (fake read functions) --------------- */

/* a canned WRITE_MSGS frame: ch 1, seq 4, count 1, CAN 7DF [02 01 00] */
static uint8_t s_wire[64];
static size_t s_wire_len;
static size_t s_wire_off;
static size_t s_piece;        /* max bytes one read() hands back */
static int s_zero_reads;      /* return 0 this many times first */
static int s_down_after;      /* return -1 once this many bytes went out (-1 = never) */
static uint32_t s_reads;

static void wire_reset(size_t piece)
{
    j2534_msg_t m = { .protocol_id = J2534_PROT_CAN, .data_size = 7,
                      .data = { 0x00, 0x00, 0x07, 0xDF, 0x02, 0x01, 0x00 } };
    uint8_t payload[64];

    payload[0] = 1; payload[1] = 0; payload[2] = 0; payload[3] = 0;

    size_t n = j2534_msg_encode(&m, payload + 4, sizeof(payload) - 4);

    s_wire_len = j2534_hdr_encode(s_wire, J2534_MT_WRITE_MSGS, 4, 1,
                                  (uint32_t)(4 + n));
    memcpy(s_wire + s_wire_len, payload, 4 + n);
    s_wire_len += 4 + n;
    s_wire_off = 0;
    s_piece = piece;
    s_zero_reads = 0;
    s_down_after = -1;
    s_reads = 0;
}

static int fake_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    s_reads++;

    if (s_zero_reads > 0)
    {
        s_zero_reads--;
        return 0;
    }

    if (s_down_after >= 0 && (int)s_wire_off >= s_down_after)
    {
        return -1;
    }

    size_t left = s_wire_len - s_wire_off;

    if (left == 0)
    {
        return 0; /* nothing more: a timeout */
    }

    if (n > left) n = left;
    if (n > s_piece) n = s_piece;
    memcpy(buf, s_wire + s_wire_off, n);
    s_wire_off += n;
    return (int)n;
}

static void test_frame_read_partial_chunks(void)
{
    static const size_t pieces[] = { 1, 7, 13, 490 };

    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++)
    {
        j2534_hdr_t h;
        uint8_t payload[J2534_RX_CAP];
        volatile bool run = true;

        wire_reset(pieces[i]);
        TEST_ASSERT_EQUAL(J2534_FR_OK,
            j2534_frame_read(fake_read, NULL, 200, 0, &run, &h, payload,
                             sizeof(payload)));
        TEST_ASSERT_EQUAL(J2534_MT_WRITE_MSGS, h.type);
        TEST_ASSERT_EQUAL(4, h.seq);
        TEST_ASSERT_EQUAL(1, h.channel);
        TEST_ASSERT_EQUAL(4 + 24 + 7, h.length);
        TEST_ASSERT_EQUAL_MEMORY(s_wire + J2534_HDR_SIZE, payload, h.length);
        TEST_ASSERT_EQUAL(s_wire_len, s_wire_off); /* consumed exactly */
    }
}

static void test_frame_read_timeout_then_data(void)
{
    j2534_hdr_t h;
    uint8_t payload[J2534_RX_CAP];

    wire_reset(8);
    s_zero_reads = 3;
    TEST_ASSERT_EQUAL(J2534_FR_OK,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));

    /* bounded idle: 3 empty polls with a budget of 2 -> TIMEOUT, nothing consumed */
    wire_reset(8);
    s_zero_reads = 3;
    TEST_ASSERT_EQUAL(J2534_FR_TIMEOUT,
        j2534_frame_read(fake_read, NULL, 200, 2, NULL, &h, payload,
                         sizeof(payload)));
    TEST_ASSERT_EQUAL(0, s_wire_off);
}

static void test_frame_read_link_down_mid_payload(void)
{
    j2534_hdr_t h;
    uint8_t payload[J2534_RX_CAP];

    wire_reset(6);
    s_down_after = J2534_HDR_SIZE + 5;
    TEST_ASSERT_EQUAL(J2534_FR_LINK_DOWN,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));
}

static void test_frame_read_bad_header(void)
{
    j2534_hdr_t h;
    uint8_t payload[J2534_RX_CAP];

    wire_reset(64);
    s_wire[0] = 0x00; /* magic */
    TEST_ASSERT_EQUAL(J2534_FR_BAD_HDR,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));

    wire_reset(64);
    s_wire[2] = 9; /* version */
    TEST_ASSERT_EQUAL(J2534_FR_BAD_HDR,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload,
                         sizeof(payload)));

    wire_reset(64);
    TEST_ASSERT_EQUAL(J2534_FR_TOO_BIG,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, payload, 10));
    TEST_ASSERT_EQUAL(J2534_HDR_SIZE, s_wire_off); /* payload untouched */
}

static void test_frame_read_discard_and_stop(void)
{
    j2534_hdr_t h;

    /* discard mode consumes exactly the frame */
    wire_reset(5);
    TEST_ASSERT_EQUAL(J2534_FR_OK,
        j2534_frame_read(fake_read, NULL, 200, 0, NULL, &h, NULL,
                         J2534_RX_CAP));
    TEST_ASSERT_EQUAL(s_wire_len, s_wire_off);
    TEST_ASSERT_EQUAL(4, h.seq);

    /* a cleared run flag stops before the first read */
    wire_reset(64);
    volatile bool run = false;
    TEST_ASSERT_EQUAL(J2534_FR_STOPPED,
        j2534_frame_read(fake_read, NULL, 200, 0, &run, &h, NULL,
                         J2534_RX_CAP));
    TEST_ASSERT_EQUAL(0, s_reads);
}

static void test_ack_encode_vectors(void)
{
    uint8_t buf[24];
    uint32_t ver = 1;

    /* HELLO ACK (seq 1, result = wire version 1): 20 bytes */
    static const uint8_t HELLO_ACK[] =
    {
        0x35, 0x4A, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(20, j2534_ack_encode(buf, sizeof(buf), 1, 0,
                                           J2534_STATUS_NOERROR, &ver));
    TEST_ASSERT_EQUAL_MEMORY(HELLO_ACK, buf, 20);

    /* refusal of a second tester (seq 4): 16 bytes, status 0x1A */
    static const uint8_t IN_USE[] =
    {
        0x35, 0x4A, 0x01, 0x80, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x1A, 0x00, 0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(16, j2534_ack_encode(buf, sizeof(buf), 4, 0,
                                           J2534_ERR_DEVICE_IN_USE, NULL));
    TEST_ASSERT_EQUAL_MEMORY(IN_USE, buf, 16);

    TEST_ASSERT_EQUAL(0, j2534_ack_encode(buf, 15, 1, 0, 0, NULL));
}

static void test_tcp_gate_predicate(void)
{
    /* allow_lan off: loopback / AP / USB pass, a STA-side landing is refused */
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(false, true, false, false));
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(false, false, true, false));
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(false, false, false, true));
    TEST_ASSERT_FALSE(j2534_tcp_gate_allowed(false, false, false, false));
    /* allow_lan on: everything */
    TEST_ASSERT_TRUE(j2534_tcp_gate_allowed(true, false, false, false));
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
    RUN_TEST(test_frame_read_partial_chunks);
    RUN_TEST(test_frame_read_timeout_then_data);
    RUN_TEST(test_frame_read_link_down_mid_payload);
    RUN_TEST(test_frame_read_bad_header);
    RUN_TEST(test_frame_read_discard_and_stop);
    RUN_TEST(test_ack_encode_vectors);
    RUN_TEST(test_tcp_gate_predicate);
    UNITY_END();
}
