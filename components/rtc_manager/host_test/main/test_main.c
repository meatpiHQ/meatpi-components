/**
 * @file test_main.c
 * @brief Host tests for rtc_manager's pure RX8130 time codec: BCD
 *        round-trip, plausibility gating (fresh board / drained caps must
 *        NOT restore the clock). Expected: 5 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "rtc_manager_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_decode_known_time(void)
{
    /* 2026-07-04 15:42:37 UTC, weekday one-hot (Saturday) */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x37, 0x42, 0x15, 0x40, 0x04, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_TRUE(rtc_time_decode(regs, &t));
    TEST_ASSERT_EQUAL_INT(37, t.tm_sec);
    TEST_ASSERT_EQUAL_INT(42, t.tm_min);
    TEST_ASSERT_EQUAL_INT(15, t.tm_hour);
    TEST_ASSERT_EQUAL_INT(4, t.tm_mday);
    TEST_ASSERT_EQUAL_INT(6, t.tm_mon);       /* July, 0-based */
    TEST_ASSERT_EQUAL_INT(126, t.tm_year);    /* 2026          */
}

static void test_encode_round_trip(void)
{
    struct tm in =
    {
        .tm_sec = 59, .tm_min = 8, .tm_hour = 23, .tm_mday = 31,
        .tm_mon = 11, .tm_year = 199, .tm_wday = 3, /* 2099-12-31 */
    };
    uint8_t regs[RTC_REGS_LEN];
    struct tm out;

    rtc_time_encode(&in, regs);
    TEST_ASSERT_EQUAL_HEX8(0x59, regs[0]);
    TEST_ASSERT_EQUAL_HEX8(0x23, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(1 << 3, regs[3]); /* one-hot weekday */
    TEST_ASSERT_EQUAL_HEX8(0x31, regs[4]);
    TEST_ASSERT_EQUAL_HEX8(0x12, regs[5]);
    TEST_ASSERT_EQUAL_HEX8(0x99, regs[6]);
    TEST_ASSERT_TRUE(rtc_time_decode(regs, &out));
    TEST_ASSERT_EQUAL_INT(in.tm_sec, out.tm_sec);
    TEST_ASSERT_EQUAL_INT(in.tm_mday, out.tm_mday);
    TEST_ASSERT_EQUAL_INT(in.tm_year, out.tm_year);
}

static void test_fresh_board_rejected(void)
{
    /* power-on reset value: 2000-01-01 00:00:00 — year 00 < 20 */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 };
    struct tm t;

    TEST_ASSERT_FALSE(rtc_time_decode(regs, &t));
}

static void test_garbage_rejected(void)
{
    const uint8_t all_ff[RTC_REGS_LEN] =
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t bad_month[RTC_REGS_LEN] =
        { 0x00, 0x00, 0x12, 0x01, 0x15, 0x13, 0x26 };
    const uint8_t bad_day[RTC_REGS_LEN] =
        { 0x00, 0x00, 0x12, 0x01, 0x00, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_FALSE(rtc_time_decode(all_ff, &t));
    TEST_ASSERT_FALSE(rtc_time_decode(bad_month, &t));
    TEST_ASSERT_FALSE(rtc_time_decode(bad_day, &t));
}

static void test_status_bits_masked(void)
{
    /* RX8130 seconds/minutes carry a status bit in bit7 — must be masked */
    const uint8_t regs[RTC_REGS_LEN] =
        { 0x80 | 0x30, 0x80 | 0x15, 0x12, 0x01, 0x04, 0x07, 0x26 };
    struct tm t;

    TEST_ASSERT_TRUE(rtc_time_decode(regs, &t));
    TEST_ASSERT_EQUAL_INT(30, t.tm_sec);
    TEST_ASSERT_EQUAL_INT(15, t.tm_min);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_known_time);
    RUN_TEST(test_encode_round_trip);
    RUN_TEST(test_fresh_board_rejected);
    RUN_TEST(test_garbage_rejected);
    RUN_TEST(test_status_bits_masked);
    UNITY_END();
}
