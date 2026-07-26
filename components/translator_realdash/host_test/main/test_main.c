/**
 * @file test_main.c
 * @brief Host suite for the pure RealDash codec: decode (CAN→66) format +
 *        CRC32, encode (66 & 44 → CAN) with checksum validation, fragmentation
 *        at every byte boundary, bad-checksum reject, resync after garbage,
 *        and 29-bit id detection.
 */
#include <string.h>

#include "unity.h"

#include "bridge_manager.h"
#include "can_frame_wire.h"

extern const bridge_translator_t translator_realdash_desc;

/* encode emits CAN-wire chunks → decode back to frames */
#define MAXF 8
static can_core_frame_t g_frames[MAXF];
static int g_nframes;
static esp_err_t frame_sink(void *a, const uint8_t *o, size_t l)
{
    (void)a;
    if (g_nframes < MAXF && can_wire_decode(o, l, &g_frames[g_nframes])) g_nframes++;
    return ESP_OK;
}
/* decode emits RealDash frames → collect raw */
static uint8_t g_out[64];
static size_t g_olen;
static esp_err_t raw_sink(void *a, const uint8_t *o, size_t l)
{
    (void)a;
    if (g_olen + l <= sizeof(g_out)) { memcpy(g_out + g_olen, o, l); g_olen += l; }
    return ESP_OK;
}
static void reset(void) { g_nframes = 0; g_olen = 0; }

static void enc(const uint8_t *b, size_t n)
{
    uint8_t ctx[32];
    translator_realdash_desc.ctx_init(ctx);
    translator_realdash_desc.encode(ctx, b, n, frame_sink, NULL);
}
static void enc_frag(const uint8_t *b, size_t n)
{
    uint8_t ctx[32];
    translator_realdash_desc.ctx_init(ctx);
    for (size_t i = 0; i < n; i++)
        translator_realdash_desc.encode(ctx, &b[i], 1, frame_sink, NULL);
}

/* build a valid 66 frame for id/data (CRC32 over first 16) */
static size_t make66(uint8_t *o, uint32_t id, const uint8_t *data, int dlc)
{
    /* reuse the codec's decode to build the frame from a CAN chunk */
    can_core_frame_t f;
    uint8_t wire[CAN_WIRE_MAX];
    memset(&f, 0, sizeof(f));
    f.id = id; f.ext = (id & 0x1FFFF800u) != 0; f.dlc = (uint8_t)dlc;
    memcpy(f.data, data, dlc);
    size_t wn = can_wire_encode(&f, wire);
    uint8_t ctx[32];
    g_olen = 0;
    translator_realdash_desc.ctx_init(ctx);
    translator_realdash_desc.decode(ctx, wire, wn, raw_sink, NULL);
    memcpy(o, g_out, g_olen);
    return g_olen;
}

void test_decode_66_format(void)
{
    reset();
    uint8_t frame[32];
    uint8_t d[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0 };
    size_t n = make66(frame, 0x100, d, 4);
    TEST_ASSERT_EQUAL(20, n);
    TEST_ASSERT_EQUAL_HEX8(0x66, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x33, frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, frame[4]); /* id 0x100 LE */
    TEST_ASSERT_EQUAL_HEX8(0x01, frame[5]);
    TEST_ASSERT_EQUAL_HEX8(0xDE, frame[8]);
}

void test_encode_66_roundtrip(void)
{
    reset();
    uint8_t frame[32];
    uint8_t d[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    size_t n = make66(frame, 0x123, d, 8);
    enc(frame, n);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x123, g_frames[0].id);
    TEST_ASSERT_EQUAL(8, g_frames[0].dlc);
    TEST_ASSERT_EQUAL_HEX8(0x88, g_frames[0].data[7]);
}

void test_encode_66_fragmented(void)
{
    reset();
    uint8_t frame[32];
    uint8_t d[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t n = make66(frame, 0x18FF0001, d, 8);
    enc_frag(frame, n);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x18FF0001, g_frames[0].id);
    TEST_ASSERT_TRUE(g_frames[0].ext); /* 29-bit */
}

void test_encode_66_bad_crc_rejected(void)
{
    reset();
    uint8_t frame[32];
    uint8_t d[8] = { 0xAA, 0, 0, 0, 0, 0, 0, 0 };
    size_t n = make66(frame, 0x200, d, 1);
    frame[19] ^= 0xFF; /* corrupt CRC */
    enc(frame, n);
    TEST_ASSERT_EQUAL(0, g_nframes);
    /* ctx recovers: a good frame right after works */
    n = make66(frame, 0x201, d, 1);
    enc(frame, n);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x201, g_frames[0].id);
}

void test_encode_44(void)
{
    reset();
    /* 44 33 22 11 <id:4 LE> <data:8> <chksum8> */
    uint8_t f[17] = { 0x44, 0x33, 0x22, 0x11,
                      0x50, 0x00, 0x00, 0x00, /* id 0x50 */
                      0xC0, 0xFF, 0xEE, 0, 0, 0, 0, 0, 0 };
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) sum += f[i];
    f[16] = (uint8_t)sum;
    enc(f, 17);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x50, g_frames[0].id);
    TEST_ASSERT_EQUAL_HEX8(0xC0, g_frames[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, g_frames[0].data[2]);
}

void test_encode_resync_after_garbage(void)
{
    reset();
    uint8_t frame[32];
    uint8_t d[8] = { 0x9, 0, 0, 0, 0, 0, 0, 0 };
    size_t n = make66(frame, 0x321, d, 1);
    uint8_t stream[40];
    stream[0] = 0x00; stream[1] = 0xFF; stream[2] = 0x66; /* garbage + false start */
    memcpy(&stream[3], frame, n);
    enc(stream, 3 + n);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x321, g_frames[0].id);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_66_format);
    RUN_TEST(test_encode_66_roundtrip);
    RUN_TEST(test_encode_66_fragmented);
    RUN_TEST(test_encode_66_bad_crc_rejected);
    RUN_TEST(test_encode_44);
    RUN_TEST(test_encode_resync_after_garbage);
    UNITY_END();
}
