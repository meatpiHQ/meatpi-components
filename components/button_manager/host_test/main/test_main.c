/**
 * @file test_main.c
 * @brief Host tests for the pure press state machine: hold threshold,
 *        one-shot per press, release re-arm, bounce resets the count.
 *        Expected output: 6 Tests 0 Failures 0 Ignored.
 */
#include "unity.h"

#include "button_manager_private.h"

static btn_press_t s_p;

void setUp(void)
{
    btn_press_reset(&s_p);
}

void tearDown(void)
{
}

static void test_short_press_never_fires(void)
{
    for (int i = 0; i < 4; i++)
    {
        TEST_ASSERT_FALSE(btn_press_step(&s_p, true, 5));
    }

    TEST_ASSERT_FALSE(btn_press_step(&s_p, false, 5)); /* released */
}

static void test_hold_fires_at_threshold(void)
{
    for (int i = 0; i < 4; i++)
    {
        TEST_ASSERT_FALSE(btn_press_step(&s_p, true, 5));
    }

    TEST_ASSERT_TRUE(btn_press_step(&s_p, true, 5)); /* 5th tick */
}

static void test_one_shot_while_held(void)
{
    for (int i = 0; i < 5; i++)
    {
        btn_press_step(&s_p, true, 5);
    }

    /* keep holding well past the threshold: never re-fires */
    for (int i = 0; i < 30; i++)
    {
        TEST_ASSERT_FALSE(btn_press_step(&s_p, true, 5));
    }
}

static void test_release_rearms(void)
{
    for (int i = 0; i < 5; i++)
    {
        btn_press_step(&s_p, true, 5);
    }

    TEST_ASSERT_FALSE(btn_press_step(&s_p, false, 5));

    for (int i = 0; i < 4; i++)
    {
        TEST_ASSERT_FALSE(btn_press_step(&s_p, true, 5));
    }

    TEST_ASSERT_TRUE(btn_press_step(&s_p, true, 5)); /* second press */
}

static void test_bounce_resets_count(void)
{
    for (int i = 0; i < 4; i++)
    {
        btn_press_step(&s_p, true, 5);
    }

    btn_press_step(&s_p, false, 5); /* blip */

    for (int i = 0; i < 4; i++)
    {
        TEST_ASSERT_FALSE(btn_press_step(&s_p, true, 5));
    }

    TEST_ASSERT_TRUE(btn_press_step(&s_p, true, 5));
}

static void test_threshold_one_tick(void)
{
    TEST_ASSERT_TRUE(btn_press_step(&s_p, true, 1));
    TEST_ASSERT_FALSE(btn_press_step(&s_p, true, 1));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_press_never_fires);
    RUN_TEST(test_hold_fires_at_threshold);
    RUN_TEST(test_one_shot_while_held);
    RUN_TEST(test_release_rearms);
    RUN_TEST(test_bounce_resets_count);
    RUN_TEST(test_threshold_one_tick);
    UNITY_END();
}
