/* Host tests for data_logger_recover.c: the pure recovery decisions
   (ROBUSTNESS.md): torn text tails, .wdl resync, corrupt names, the
   PSRAM salvage sanity checks, the CRC, and an open against the internal
   heap (the floor and the fault's detail, case 14). */
#include <string.h>

#include "unity.h"

#include "data_logger_private.h"

void test_text_keep_whole_when_newline_terminated(void)
{
    const char *s = "ts_ms,param,value\n1,2,3\n";

    TEST_ASSERT_EQUAL_UINT(strlen(s), dl_recover_text_keep(s, strlen(s)));
}

void test_text_keep_drops_the_torn_last_line(void)
{
    const char *s = "1,2,3\n4,5,6\n7,8";

    TEST_ASSERT_EQUAL_UINT(12, dl_recover_text_keep(s, strlen(s)));
    TEST_ASSERT_EQUAL_UINT(0, dl_recover_text_keep("no newline at all", 17));
    TEST_ASSERT_EQUAL_UINT(0, dl_recover_text_keep("", 0));
}

void test_wdl_scan_counts_complete_records_only(void)
{
    uint8_t buf[64];
    size_t n = 0;
    bool bad = true;

    /* def "a.b" (id 1), a param (19 B), a 3-byte frame (14 + 3) */
    buf[n++] = 0x01; buf[n++] = 1; buf[n++] = 0; buf[n++] = 3;
    memcpy(&buf[n], "a.b", 3); n += 3;
    buf[n++] = 0x02; memset(&buf[n], 0, 18); n += 18;
    size_t frame_at = n;
    buf[n++] = 0x03; memset(&buf[n], 0, 12); n += 12; buf[n++] = 3;
    buf[n++] = 0xAA; buf[n++] = 0xBB; buf[n++] = 0xCC;

    TEST_ASSERT_EQUAL_UINT(n, dl_recover_wdl_scan(buf, n, &bad));
    TEST_ASSERT_FALSE(bad);
    /* one data byte missing from the frame: the frame is not counted */
    TEST_ASSERT_EQUAL_UINT(frame_at, dl_recover_wdl_scan(buf, n - 1, &bad));
    TEST_ASSERT_FALSE(bad);
    /* cut inside the frame header (dlc byte not there yet) */
    TEST_ASSERT_EQUAL_UINT(frame_at, dl_recover_wdl_scan(buf, frame_at + 5, &bad));
    TEST_ASSERT_FALSE(bad);
}

void test_wdl_scan_flags_garbage(void)
{
    uint8_t buf[8] = { 0x02, 0, 0, 0, 0, 0, 0, 0 };
    bool bad = false;

    /* a partial param record is just partial … */
    TEST_ASSERT_EQUAL_UINT(0, dl_recover_wdl_scan(buf, sizeof(buf), &bad));
    TEST_ASSERT_FALSE(bad);
    /* … an unknown type byte is garbage */
    buf[0] = 0x7F;
    TEST_ASSERT_EQUAL_UINT(0, dl_recover_wdl_scan(buf, sizeof(buf), &bad));
    TEST_ASSERT_TRUE(bad);
    /* a frame claiming 9 data bytes is garbage too */
    uint8_t f[16] = { 0x03 };
    f[13] = 9;
    TEST_ASSERT_EQUAL_UINT(0, dl_recover_wdl_scan(f, sizeof(f), &bad));
    TEST_ASSERT_TRUE(bad);
}

void test_corrupt_names_round_trip(void)
{
    char out[64];
    int64_t epoch = 0;

    TEST_ASSERT_TRUE(dl_recover_corrupt_name("dl_1784563545.db", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("dl_1784563545.db.corrupt", out);
    TEST_ASSERT_TRUE(dl_recover_is_corrupt_name(out, "dl_", &epoch));
    TEST_ASSERT_EQUAL_INT64(1784563545, epoch);
    TEST_ASSERT_FALSE(dl_recover_is_corrupt_name(out, "can_", NULL));
    TEST_ASSERT_FALSE(dl_recover_is_corrupt_name("dl_1784563545.db", "dl_", NULL));
    TEST_ASSERT_FALSE(dl_recover_is_corrupt_name("notes.corrupt", NULL, NULL));
    TEST_ASSERT_FALSE(dl_recover_corrupt_name("dl_1784563545.db", out, 10));
}

void test_record_sanity(void)
{
    dl_record_t p = { .ts_ms = 1784563545000LL, .kind = DL_REC_PARAM,
                      .u.p = { .value = 1.5, .param = 3 } };
    dl_record_t f = { .ts_ms = 0, .kind = DL_REC_FRAME,
                      .u.f = { .id = 0x7E8, .dlc = 8, .flags = DL_FRAME_EXT } };

    TEST_ASSERT_TRUE(dl_recover_record_sane(&p));
    TEST_ASSERT_TRUE(dl_recover_record_sane(&f));
    p.u.p.param = DL_MAX_PARAMS;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&p));
    p.u.p.param = 0; p.ts_ms = -1;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&p));
    p.ts_ms = 4000000000001LL;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&p));
    f.u.f.dlc = 9;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&f));
    f.u.f.dlc = 8; f.u.f.flags = 0x10;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&f));
    f.u.f.flags = 0; f.kind = 7;
    TEST_ASSERT_FALSE(dl_recover_record_sane(&f));
}

void test_ring_sanity_and_fill_recovery(void)
{
    uint32_t fill = 3;

    TEST_ASSERT_TRUE(dl_recover_ring_sane(8, 8, 5, 2, &fill));
    TEST_ASSERT_EQUAL_UINT32(3, fill);
    /* the trio disagrees (reset between two index writes): trust head/tail */
    fill = 9;
    TEST_ASSERT_TRUE(dl_recover_ring_sane(8, 8, 5, 2, &fill));
    TEST_ASSERT_EQUAL_UINT32(3, fill);
    fill = 2;
    TEST_ASSERT_TRUE(dl_recover_ring_sane(8, 8, 1, 6, &fill));
    TEST_ASSERT_EQUAL_UINT32(3, fill);
    /* a full ring is the one legitimate head == tail with fill == cap */
    fill = 8;
    TEST_ASSERT_TRUE(dl_recover_ring_sane(8, 8, 4, 4, &fill));
    TEST_ASSERT_EQUAL_UINT32(8, fill);
    /* head == tail otherwise means empty */
    fill = 5;
    TEST_ASSERT_TRUE(dl_recover_ring_sane(8, 8, 4, 4, &fill));
    TEST_ASSERT_EQUAL_UINT32(0, fill);
    /* garbage indices */
    fill = 0;
    TEST_ASSERT_FALSE(dl_recover_ring_sane(8, 8, 8, 0, &fill));
    TEST_ASSERT_FALSE(dl_recover_ring_sane(0, 8, 0, 0, &fill));
    TEST_ASSERT_FALSE(dl_recover_ring_sane(16, 8, 0, 0, &fill));
    TEST_ASSERT_FALSE(dl_recover_ring_sane(8, 8, 0, 0, NULL));
}

void test_crc32_known_vector(void)
{
    /* CRC-32/ISO-HDLC of "123456789" */
    uint32_t crc = dl_recover_crc32_update(0, "123456789", 9);

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
    /* incremental == one shot */
    uint32_t a = dl_recover_crc32_update(0, "1234", 4);
    TEST_ASSERT_EQUAL_HEX32(crc, dl_recover_crc32_update(a, "56789", 5));
}

/* ---- an open and the internal heap (ROBUSTNESS.md case 14) ---------------- */

void test_open_floor_is_one_kilobyte_in_one_block(void)
{
    /* the field report of 2026-11-13: 5.3 KB free, the largest block
       3.6 KB. With the stdio buffer kept such a unit opens its file */
    TEST_ASSERT_TRUE(dl_recover_open_ram_ok(3686));
    TEST_ASSERT_TRUE(dl_recover_open_ram_ok(DL_OPEN_RAM_MIN));
    /* under the floor no open is tried: fopen() would abort the unit when
       the C library cannot have its lock (the crash of 2026-11-07) */
    TEST_ASSERT_FALSE(dl_recover_open_ram_ok(DL_OPEN_RAM_MIN - 1));
    TEST_ASSERT_FALSE(dl_recover_open_ram_ok(512));
    TEST_ASSERT_FALSE(dl_recover_open_ram_ok(0));
}

void test_corrupt_is_no_verdict_under_the_floor(void)
{
    /* the engine says corrupt and the unit can read its card: the file is
       set aside (ROBUSTNESS.md case 5) */
    TEST_ASSERT_TRUE(dl_recover_corrupt_counts(true, 31744));
    TEST_ASSERT_TRUE(dl_recover_corrupt_counts(true, DL_OPEN_RAM_MIN));
    /* under the floor the card reads fail for want of a bounce buffer and
       sqlite calls the zeros "not a database": a good file is not touched */
    TEST_ASSERT_FALSE(dl_recover_corrupt_counts(true, DL_OPEN_RAM_MIN - 1));
    TEST_ASSERT_FALSE(dl_recover_corrupt_counts(true, 12));
    TEST_ASSERT_FALSE(dl_recover_corrupt_counts(true, 0));
    /* and a file the engine did not call corrupt never is */
    TEST_ASSERT_FALSE(dl_recover_corrupt_counts(false, 31744));
    TEST_ASSERT_FALSE(dl_recover_corrupt_counts(false, 0));
}

void test_fault_detail_tells_the_memory_from_the_card(void)
{
    char out[48];

    /* the card: the detail as it has been since 2026-09-07 */
    TEST_ASSERT_EQUAL_INT(6, dl_recover_fault_detail(out, sizeof(out), false, false));
    TEST_ASSERT_EQUAL_STRING("params", out);
    TEST_ASSERT_EQUAL_INT(3, dl_recover_fault_detail(out, sizeof(out), true, false));
    TEST_ASSERT_EQUAL_STRING("can", out);
    /* the internal heap: named, so nobody looks for a fault on a good card */
    TEST_ASSERT_EQUAL_INT(27, dl_recover_fault_detail(out, sizeof(out), false, true));
    TEST_ASSERT_EQUAL_STRING("params: out of internal RAM", out);
    TEST_ASSERT_EQUAL_INT(24, dl_recover_fault_detail(out, sizeof(out), true, true));
    TEST_ASSERT_EQUAL_STRING("can: out of internal RAM", out);
    /* it fits the fault store's 48-byte detail; a buffer too small for it
       answers 0 and never a cut text read as the other reason */
    TEST_ASSERT_TRUE(strlen("params: out of internal RAM") < 48);
    TEST_ASSERT_EQUAL_INT(0, dl_recover_fault_detail(out, 10, false, true));
    TEST_ASSERT_EQUAL_STRING("", out);
}
