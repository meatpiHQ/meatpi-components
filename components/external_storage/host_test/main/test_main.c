/**
 * @file test_main.c
 * @brief Host tests for the pure card-detect debouncer: glitch rejection,
 *        clean insert/remove edges, agreement-resets-pending-change.
 */
#include "unity.h"

#include "external_storage_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_glitches_never_surface(void)
{
    es_detect_t d;

    es_detect_init(&d, false);

    /* 1..3 present samples then back: below the threshold, no change */
    for (int burst = 1; burst < ES_DEBOUNCE_SAMPLES; burst++)
    {
        for (int i = 0; i < burst; i++)
        {
            TEST_ASSERT_FALSE(es_detect_feed(&d, true));
        }

        TEST_ASSERT_FALSE(es_detect_feed(&d, false)); /* back to agree */
        TEST_ASSERT_FALSE(d.stable_present);
    }
}

static void test_clean_insert_and_remove(void)
{
    es_detect_t d;

    es_detect_init(&d, false);

    for (int i = 0; i < ES_DEBOUNCE_SAMPLES - 1; i++)
    {
        TEST_ASSERT_FALSE(es_detect_feed(&d, true));
    }

    TEST_ASSERT_TRUE(es_detect_feed(&d, true)); /* the edge fires ONCE */
    TEST_ASSERT_TRUE(d.stable_present);
    TEST_ASSERT_FALSE(es_detect_feed(&d, true)); /* steady: no repeat */

    for (int i = 0; i < ES_DEBOUNCE_SAMPLES - 1; i++)
    {
        TEST_ASSERT_FALSE(es_detect_feed(&d, false));
    }

    TEST_ASSERT_TRUE(es_detect_feed(&d, false));
    TEST_ASSERT_FALSE(d.stable_present);
}

static void test_bouncing_insertion_settles(void)
{
    es_detect_t d;

    es_detect_init(&d, false);

    /* seating wobble: alternating samples never accumulate */
    for (int i = 0; i < 6; i++)
    {
        TEST_ASSERT_FALSE(es_detect_feed(&d, (i % 2) == 0));
    }

    /* then it settles: exactly ES_DEBOUNCE_SAMPLES to flip. The wobble
       ended on a present sample (i=4 true, i=5 false, so candidate=false)
       -> a full fresh run of present samples is required */
    int flips = 0;

    for (int i = 0; i < ES_DEBOUNCE_SAMPLES; i++)
    {
        if (es_detect_feed(&d, true))
        {
            flips++;
        }
    }

    TEST_ASSERT_EQUAL(1, flips);
    TEST_ASSERT_TRUE(d.stable_present);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_glitches_never_surface);
    RUN_TEST(test_clean_insert_and_remove);
    RUN_TEST(test_bouncing_insertion_settles);
    UNITY_END();
}
