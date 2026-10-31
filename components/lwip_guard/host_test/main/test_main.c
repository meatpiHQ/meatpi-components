/**
 * @file test_main.c
 * @brief Host tests for lwip_guard's pure core: the refusal counter and
 *        the one-line-per-period log rate limit.
 *        Expected output: 5 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "lwip_guard_private.h"

static lwip_guard_state_t s_st;

void setUp(void)
{
    memset(&s_st, 0, sizeof(s_st));
}

void tearDown(void)
{
}

static void test_first_refusal_is_logged(void)
{
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, s_st.refusals);
    TEST_ASSERT_EQUAL_UINT32(5000, s_st.last_log_ms);
}

static void test_a_second_refusal_inside_the_period_is_counted_not_logged(void)
{
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, 5000));
    TEST_ASSERT_FALSE(lwip_guard_note(&s_st, 5000 + LWIP_GUARD_LOG_PERIOD_MS - 1));
    TEST_ASSERT_EQUAL_UINT32(2, s_st.refusals);
    TEST_ASSERT_EQUAL_UINT32(5000, s_st.last_log_ms);
}

static void test_one_line_per_period(void)
{
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, 0));
    TEST_ASSERT_FALSE(lwip_guard_note(&s_st, 3000));
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, LWIP_GUARD_LOG_PERIOD_MS));
    TEST_ASSERT_FALSE(lwip_guard_note(&s_st, LWIP_GUARD_LOG_PERIOD_MS + 9999));
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, 2 * LWIP_GUARD_LOG_PERIOD_MS + 1));
    TEST_ASSERT_EQUAL_UINT32(5, s_st.refusals);
}

static void test_every_refusal_counts(void)
{
    for (uint32_t i = 0; i < 1000; i++)
    {
        (void)lwip_guard_note(&s_st, i);
    }

    TEST_ASSERT_EQUAL_UINT32(1000, s_st.refusals);
}

static void test_wrapping_clock(void)
{
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, 0xFFFFF000u));
    TEST_ASSERT_FALSE(lwip_guard_note(&s_st, 0xFFFFFFF0u));
    /* 0x1000 past the wrap: more than the period since 0xFFFFF000 */
    TEST_ASSERT_TRUE(lwip_guard_note(&s_st, 0x2000u));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_refusal_is_logged);
    RUN_TEST(test_a_second_refusal_inside_the_period_is_counted_not_logged);
    RUN_TEST(test_one_line_per_period);
    RUN_TEST(test_every_refusal_counts);
    RUN_TEST(test_wrapping_clock);
    UNITY_END();
}
