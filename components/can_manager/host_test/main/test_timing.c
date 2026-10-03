/**
 * @file test_timing.c
 * @brief Host suite for can_timing_core.c: the bit timing the native CAN
 *        node asks for. The source clock of the bench device is 80 MHz
 *        (ESP32-S3 APB). Run from test_main.c's app_main.
 */
#include <stddef.h>

#include "unity.h"

#include "can_timing_core.h"

#define APB_HZ 80000000u

static void check_shape(const can_timing_t *t)
{
    TEST_ASSERT_EQUAL_UINT8(15, t->tseg1);
    TEST_ASSERT_EQUAL_UINT8(4, t->tseg2);
    TEST_ASSERT_EQUAL_UINT8(3, t->sjw);
    TEST_ASSERT_EQUAL_UINT32(0, t->brp & 1u);         /* even prescaler   */

    /* 20 quanta, the sample point after the sync quantum + TSEG1: 80 % */
    TEST_ASSERT_EQUAL_UINT32(CAN_TIMING_QUANTA, 1u + t->tseg1 + t->tseg2);
    TEST_ASSERT_EQUAL_UINT32(800, (1u + t->tseg1) * 1000u / CAN_TIMING_QUANTA);
    TEST_ASSERT_TRUE(t->sjw <= t->tseg2);
}

void test_timing_every_setting_value_at_80mhz(void)
{
    /* the values of can_manager's "baud" setting and their prescalers */
    static const struct { uint16_t kbps; uint32_t brp; uint32_t bps; } T[] =
    {
        { 33, 120, 33333 }, { 83, 48, 83333 }, { 95, 42, 95238 },
        { 100, 40, 100000 }, { 125, 32, 125000 }, { 250, 16, 250000 },
        { 500, 8, 500000 }, { 1000, 4, 1000000 },
    };

    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
    {
        can_timing_t t = { 0 };

        TEST_ASSERT_TRUE(can_timing_for(APB_HZ, T[i].kbps, &t));
        TEST_ASSERT_EQUAL_UINT32(T[i].brp, t.brp);
        TEST_ASSERT_EQUAL_UINT32(T[i].bps, t.bitrate);
        check_shape(&t);
    }
}

void test_timing_250k_is_not_the_drivers_choice(void)
{
    /* esp_driver_twai on its own: brp 16, 16 quanta before the sample
       point, 3 after, sjw 1 (85 %). Ours, the one the bench read 20965
       frames with and not one error: */
    can_timing_t t = { 0 };

    TEST_ASSERT_TRUE(can_timing_for(APB_HZ, 250, &t));
    TEST_ASSERT_EQUAL_UINT32(16, t.brp);
    TEST_ASSERT_EQUAL_UINT8(15, t.tseg1);
    TEST_ASSERT_EQUAL_UINT8(4, t.tseg2);
    TEST_ASSERT_EQUAL_UINT8(3, t.sjw);
}

void test_timing_nominal_rates(void)
{
    TEST_ASSERT_EQUAL_UINT32(33333, can_timing_nominal_bps(33));
    TEST_ASSERT_EQUAL_UINT32(83333, can_timing_nominal_bps(83));
    TEST_ASSERT_EQUAL_UINT32(95238, can_timing_nominal_bps(95));
    TEST_ASSERT_EQUAL_UINT32(500000, can_timing_nominal_bps(500));
    TEST_ASSERT_EQUAL_UINT32(50000, can_timing_nominal_bps(50));
    TEST_ASSERT_EQUAL_UINT32(0, can_timing_nominal_bps(0));
}

void test_timing_refuses_what_the_clock_cannot_make(void)
{
    can_timing_t t = { .brp = 77 };

    /* 800 kbit/s from 80 MHz: prescaler 5, odd */
    TEST_ASSERT_FALSE(can_timing_for(APB_HZ, 800, &t));
    TEST_ASSERT_EQUAL_UINT32(77, t.brp);              /* untouched        */

    /* 1 Mbit/s from 20 MHz: prescaler 1, below the minimum */
    TEST_ASSERT_FALSE(can_timing_for(20000000u, 1000, &t));

    /* 500 kbit/s from 77 MHz: 7.7 rounds to 8, 481 kbit/s is 3.8 % off */
    TEST_ASSERT_FALSE(can_timing_for(77000000u, 500, &t));

    TEST_ASSERT_FALSE(can_timing_for(0, 500, &t));
    TEST_ASSERT_FALSE(can_timing_for(APB_HZ, 0, &t));
    TEST_ASSERT_FALSE(can_timing_for(APB_HZ, 500, NULL));
}

void test_timing_other_clocks(void)
{
    can_timing_t t = { 0 };

    /* a 40 MHz crystal as the source */
    TEST_ASSERT_TRUE(can_timing_for(40000000u, 500, &t));
    TEST_ASSERT_EQUAL_UINT32(4, t.brp);
    check_shape(&t);
    TEST_ASSERT_TRUE(can_timing_for(40000000u, 1000, &t));
    TEST_ASSERT_EQUAL_UINT32(2, t.brp);
    TEST_ASSERT_TRUE(can_timing_for(40000000u, 250, &t));
    TEST_ASSERT_EQUAL_UINT32(8, t.brp);
    TEST_ASSERT_EQUAL_UINT32(250000, t.bitrate);

    /* slow rates stay inside the prescaler's range */
    TEST_ASSERT_TRUE(can_timing_for(APB_HZ, 25, &t));
    TEST_ASSERT_EQUAL_UINT32(160, t.brp);
    check_shape(&t);
}

void run_timing_tests(void)
{
    RUN_TEST(test_timing_every_setting_value_at_80mhz);
    RUN_TEST(test_timing_250k_is_not_the_drivers_choice);
    RUN_TEST(test_timing_nominal_rates);
    RUN_TEST(test_timing_refuses_what_the_clock_cannot_make);
    RUN_TEST(test_timing_other_clocks);
}
