/**
 * @file test_main.c
 * @brief Host tests for can_selftest's pure half: the chip's answer to the
 *        engine speed request classified, the rpm decoded, the verdict.
 */
#include <string.h>

#include "unity.h"

#include "can_selftest.h"

static void test_ours_headers_on(void)
{
    int rpm = -1;

    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_OURS,
                      can_selftest_parse_reply("7EF 04 41 0C 1A F8\r", 0x7EF, &rpm));
    TEST_ASSERT_EQUAL(1726, rpm);
}

static void test_ours_after_other_ecus(void)
{
    int rpm = -1;
    const char *resp = "7E8 04 41 0C 0B 5C\r7E9 04 41 0C 0B 60\r7EF 04 41 0C 1A F8\r";

    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_OURS,
                      can_selftest_parse_reply(resp, 0x7EF, &rpm));
    TEST_ASSERT_EQUAL(1726, rpm);
}

static void test_other_ecus_only(void)
{
    int rpm = -1;

    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_OTHER,
                      can_selftest_parse_reply("7E8 04 41 0C 0B 5C\r", 0x7EF, &rpm));
    TEST_ASSERT_EQUAL(-1, rpm);
}

static void test_no_data_and_errors(void)
{
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_NODATA,
                      can_selftest_parse_reply("NO DATA\r", 0x7EF, NULL));
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_CANERROR,
                      can_selftest_parse_reply("CAN ERROR\r", 0x7EF, NULL));
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_CANERROR,
                      can_selftest_parse_reply("BUS ERROR\r", 0x7EF, NULL));
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_ERROR,
                      can_selftest_parse_reply("?\r", 0x7EF, NULL));
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_ERROR,
                      can_selftest_parse_reply("UNABLE TO CONNECT\r", 0x7EF, NULL));
}

static void test_empty_and_noise(void)
{
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_NONE, can_selftest_parse_reply("", 0x7EF, NULL));
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_NONE, can_selftest_parse_reply(NULL, 0x7EF, NULL));
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_NONE,
                      can_selftest_parse_reply("SEARCHING...\r", 0x7EF, NULL));
}

static void test_wrong_pid_is_not_ours(void)
{
    /* a 7EF line that is not the engine speed reply (another PID) */
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_NONE,
                      can_selftest_parse_reply("7EF 06 41 00 BE 3E B8 13\r", 0x7EF, NULL));
}

static void test_can_error_wins_over_no_data(void)
{
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_CANERROR,
                      can_selftest_parse_reply("NO DATA\rCAN ERROR\r", 0x7EF, NULL));
}

static void test_rpm_decode_edges(void)
{
    int rpm = -1;

    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_OURS,
                      can_selftest_parse_reply("7EF 04 41 0C 00 00\r", 0x7EF, &rpm));
    TEST_ASSERT_EQUAL(0, rpm);
    TEST_ASSERT_EQUAL(CAN_SELFTEST_REPLY_OURS,
                      can_selftest_parse_reply("7EF 04 41 0C FF FF\r", 0x7EF, &rpm));
    TEST_ASSERT_EQUAL(16383, rpm);
}

static void test_verdicts(void)
{
    bool pass = false;

    TEST_ASSERT_EQUAL_STRING("PASS: both transceivers see each other",
                             can_selftest_verdict(true, CAN_SELFTEST_REPLY_OURS, &pass));
    TEST_ASSERT_TRUE(pass);

    /* nothing acknowledged the chip: the wires */
    TEST_ASSERT_NOT_NULL(strstr(can_selftest_verdict(false, CAN_SELFTEST_REPLY_CANERROR, &pass), "wires"));
    TEST_ASSERT_FALSE(pass);
    /* the ESP32 deaf while others answered: its transceiver */
    TEST_ASSERT_NOT_NULL(strstr(can_selftest_verdict(false, CAN_SELFTEST_REPLY_OTHER, &pass), "ESP32's transceiver"));
    /* nobody heard the chip */
    TEST_ASSERT_NOT_NULL(strstr(can_selftest_verdict(false, CAN_SELFTEST_REPLY_NODATA, &pass), "chip's CAN side"));
    /* the ESP32 heard and answered, the chip never printed it */
    TEST_ASSERT_NOT_NULL(strstr(can_selftest_verdict(true, CAN_SELFTEST_REPLY_NODATA, &pass), "transmit path"));
    TEST_ASSERT_FALSE(pass);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ours_headers_on);
    RUN_TEST(test_ours_after_other_ecus);
    RUN_TEST(test_other_ecus_only);
    RUN_TEST(test_no_data_and_errors);
    RUN_TEST(test_empty_and_noise);
    RUN_TEST(test_wrong_pid_is_not_ours);
    RUN_TEST(test_can_error_wins_over_no_data);
    RUN_TEST(test_rpm_decode_edges);
    RUN_TEST(test_verdicts);
    UNITY_END();
}
