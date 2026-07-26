/**
 * @file test_main.c
 * @brief Host suite for the pure GVRET codec: decode (CAN→F1 00 record with
 *        XOR checksum + ext bit31), encode BUILD_CAN_FRAME → CAN (incl.
 *        fragmentation), and handshake replies (GET_DEV_INFO / TIME_SYNC /
 *        KEEPALIVE / GET_NUMBUSES) delivered through the reply channel.
 */
#include <string.h>

#include "unity.h"

#include "bridge_manager.h"
#include "can_frame_wire.h"

extern const bridge_translator_t translator_gvret_desc;

/* the codec ctx begins with a bridge_reply_hdr_t (wants_reply). Give the ctx
 * plenty of room and bind the reply header to a collector. */
static uint8_t g_ctx[256];

/* far-sink (decode output = GVRET records; encode output = CAN-wire) */
static uint8_t g_far[64]; static size_t g_farlen;
static can_core_frame_t g_frame; static int g_nframes;
static esp_err_t far_raw(void *a, const uint8_t *o, size_t l)
{ (void)a; if (g_farlen + l <= sizeof(g_far)) { memcpy(g_far + g_farlen, o, l); g_farlen += l; } return ESP_OK; }
static esp_err_t far_frame(void *a, const uint8_t *o, size_t l)
{ (void)a; if (can_wire_decode(o, l, &g_frame)) g_nframes++; return ESP_OK; }

/* reply channel collector */
static uint8_t g_reply[64]; static size_t g_replylen;
static esp_err_t reply_sink(void *a, const uint8_t *o, size_t l)
{ (void)a; if (g_replylen + l <= sizeof(g_reply)) { memcpy(g_reply + g_replylen, o, l); g_replylen += l; } return ESP_OK; }

static void init_ctx(void)
{
    translator_gvret_desc.ctx_init(g_ctx);
    bridge_reply_hdr_t *h = (bridge_reply_hdr_t *)g_ctx;
    h->reply = reply_sink;
    h->reply_arg = NULL;
    g_farlen = 0; g_replylen = 0; g_nframes = 0;
}

/* ---- decode ------------------------------------------------------------ */

void test_decode_frame_record(void)
{
    init_ctx();
    can_core_frame_t f = { .id = 0x100, .ext = false, .dlc = 2,
                             .data = { 0xAA, 0xBB } };
    uint8_t wire[CAN_WIRE_MAX];
    size_t n = can_wire_encode(&f, wire);
    translator_gvret_desc.decode(g_ctx, wire, n, far_raw, NULL);
    /* F1 00 <ts:4> <id:4> <dlc> <data:2> <chk> = 2+4+4+1+2+1 = 14 */
    TEST_ASSERT_EQUAL(14, g_farlen);
    TEST_ASSERT_EQUAL_HEX8(0xF1, g_far[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_far[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_far[6]);  /* id LE 0x100 */
    TEST_ASSERT_EQUAL_HEX8(0x01, g_far[7]);
    TEST_ASSERT_EQUAL_HEX8(2, g_far[10]);    /* dlc */
    TEST_ASSERT_EQUAL_HEX8(0xAA, g_far[11]);
    /* checksum = XOR of the first 13 bytes */
    uint8_t chk = 0; for (int i = 0; i < 13; i++) chk ^= g_far[i];
    TEST_ASSERT_EQUAL_HEX8(chk, g_far[13]);
}

void test_decode_ext_bit31(void)
{
    init_ctx();
    can_core_frame_t f = { .id = 0x18FF0001, .ext = true, .dlc = 0 };
    uint8_t wire[CAN_WIRE_MAX];
    size_t n = can_wire_encode(&f, wire);
    translator_gvret_desc.decode(g_ctx, wire, n, far_raw, NULL);
    uint32_t id = (uint32_t)g_far[6] | ((uint32_t)g_far[7] << 8) |
                  ((uint32_t)g_far[8] << 16) | ((uint32_t)g_far[9] << 24);
    TEST_ASSERT_TRUE((id & (1u << 31)) != 0);     /* ext bit set */
    TEST_ASSERT_EQUAL_HEX32(0x18FF0001, id & 0x7FFFFFFF);
}

/* ---- encode: BUILD_CAN_FRAME ------------------------------------------- */

void test_encode_build_frame(void)
{
    init_ctx();
    /* F1 00 <id:4 LE> <bus:1> <dlc:1> <data> <chk:1> */
    uint8_t cmd[] = { 0xF1, 0x00, 0x00, 0x01, 0x00, 0x00, /* id 0x100 */
                      0x00,                                /* bus */
                      0x03,                                /* dlc 3 */
                      0xDE, 0xAD, 0xBE,                    /* data */
                      0x00 };                              /* checksum (ignored) */
    translator_gvret_desc.encode(g_ctx, cmd, sizeof(cmd), far_frame, NULL);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x100, g_frame.id);
    TEST_ASSERT_EQUAL(3, g_frame.dlc);
    TEST_ASSERT_EQUAL_HEX8(0xBE, g_frame.data[2]);
}

void test_encode_build_frame_fragmented(void)
{
    init_ctx();
    uint8_t cmd[] = { 0xF1, 0x00, 0x34, 0x12, 0x00, 0x80, /* id bytes LE; 0x80
                                                             = bit31 ext flag */
                      0x00, 0x01, 0x99, 0x00 };
    for (size_t i = 0; i < sizeof(cmd); i++)
        translator_gvret_desc.encode(g_ctx, &cmd[i], 1, far_frame, NULL);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_TRUE(g_frame.ext);         /* bit31 → extended */
    TEST_ASSERT_EQUAL_HEX32(0x1234, g_frame.id); /* bit31 masked off */
    TEST_ASSERT_EQUAL_HEX8(0x99, g_frame.data[0]);
}

/* ---- encode: handshake replies ---------------------------------------- */

void test_reply_dev_info(void)
{
    init_ctx();
    uint8_t cmd[] = { 0xF1, 0x07 }; /* GET_DEV_INFO */
    translator_gvret_desc.encode(g_ctx, cmd, sizeof(cmd), far_frame, NULL);
    TEST_ASSERT_EQUAL(8, g_replylen);
    TEST_ASSERT_EQUAL_HEX8(0xF1, g_reply[0]);
    TEST_ASSERT_EQUAL_HEX8(7, g_reply[1]);
    TEST_ASSERT_EQUAL(0, g_nframes); /* no CAN frame */
}

void test_reply_keepalive_and_numbuses(void)
{
    init_ctx();
    uint8_t cmd[] = { 0xF1, 0x09, 0xF1, 0x0C }; /* keepalive then get_numbuses */
    translator_gvret_desc.encode(g_ctx, cmd, sizeof(cmd), far_frame, NULL);
    /* F1 09 DE AD (4) + F1 0C 01 (3) = 7 */
    TEST_ASSERT_EQUAL(7, g_replylen);
    TEST_ASSERT_EQUAL_HEX8(0xDE, g_reply[2]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, g_reply[3]);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_reply[6]);
}

void test_reply_time_sync(void)
{
    init_ctx();
    uint8_t cmd[] = { 0xF1, 0x01 };
    translator_gvret_desc.encode(g_ctx, cmd, sizeof(cmd), far_frame, NULL);
    TEST_ASSERT_EQUAL(6, g_replylen);
    TEST_ASSERT_EQUAL_HEX8(0xF1, g_reply[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_reply[1]);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_frame_record);
    RUN_TEST(test_decode_ext_bit31);
    RUN_TEST(test_encode_build_frame);
    RUN_TEST(test_encode_build_frame_fragmented);
    RUN_TEST(test_reply_dev_info);
    RUN_TEST(test_reply_keepalive_and_numbuses);
    RUN_TEST(test_reply_time_sync);
    UNITY_END();
}
