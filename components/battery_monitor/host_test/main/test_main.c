/**
 * @file test_main.c
 * @brief Host tests for battery_monitor's pure watch policy: initial-
 *        state delivery, hysteresis band, hold debounce, clock wrap.
 *        Expected output: 6 Tests 0 Failures 0 Ignored.
 */
#include "unity.h"

#include "battery_monitor_private.h"

static bm_watch_state_t s_w;

void setUp(void)
{
    /* sleep-manager-style watch: below 12.0, recover at 12.5, 5 s hold */
    TEST_ASSERT_TRUE(bm_watch_init(&s_w, 12.0f, 12.5f, 5000, 1000));
}

void tearDown(void)
{
}

static void test_init_rejects_inverted_pair(void)
{
    bm_watch_state_t w;

    TEST_ASSERT_FALSE(bm_watch_init(&w, 12.5f, 12.0f, 1000, 0));
}

static void test_initial_state_delivered_after_hold(void)
{
    /* steady 11.5 V (the bench PSU): BELOW lands once the hold passes */
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 11.5f, 1000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 11.5f, 4000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_BELOW, bm_watch_eval(&s_w, 11.5f, 6000));
    /* settled: no repeats */
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 11.5f, 9000));
}

static void test_hysteresis_band_is_silent(void)
{
    bm_watch_eval(&s_w, 11.5f, 1000);
    bm_watch_eval(&s_w, 11.5f, 6001);  /* settled BELOW */
    /* 12.2 V sits between below_v and above_v: nothing pends */
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.2f, 7000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.2f, 60000));
    /* full recovery: ABOVE after the hold */
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.6f, 61000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_ABOVE, bm_watch_eval(&s_w, 12.6f, 66000));
}

static void test_debounce_broken_by_band_dip(void)
{
    bm_watch_eval(&s_w, 11.5f, 1000);
    bm_watch_eval(&s_w, 11.5f, 6001);  /* settled BELOW */
    /* recovery starts... */
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.6f, 7000));
    /* ...but dips into the band: the hold restarts */
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.3f, 9000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.6f, 10000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE, bm_watch_eval(&s_w, 12.6f, 14000));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_ABOVE, bm_watch_eval(&s_w, 12.6f, 15000));
}

static void test_zero_hold_fires_immediately(void)
{
    bm_watch_state_t w;

    TEST_ASSERT_TRUE(bm_watch_init(&w, 12.0f, 12.5f, 0, 0));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_BELOW, bm_watch_eval(&w, 11.5f, 1));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_ABOVE, bm_watch_eval(&w, 13.0f, 2));
}

static void test_hold_clock_wrap(void)
{
    bm_watch_state_t w;

    TEST_ASSERT_TRUE(bm_watch_init(&w, 12.0f, 12.5f, 5000, 0xFFFFF000u));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE,
                          bm_watch_eval(&w, 11.5f, 0xFFFFF000u));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_NONE,
                          bm_watch_eval(&w, 11.5f, 0xFFFFFC00u));
    TEST_ASSERT_EQUAL_INT(BM_EVAL_BELOW,
                          bm_watch_eval(&w, 11.5f, 0x00000C00u));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_inverted_pair);
    RUN_TEST(test_initial_state_delivered_after_hold);
    RUN_TEST(test_hysteresis_band_is_silent);
    RUN_TEST(test_debounce_broken_by_band_dip);
    RUN_TEST(test_zero_hold_fires_immediately);
    RUN_TEST(test_hold_clock_wrap);
    UNITY_END();
}
