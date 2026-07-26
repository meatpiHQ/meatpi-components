/**
 * @file test_main.c
 * @brief Host tests for the pure USB ID-pin debouncer: boot states,
 *        stable attach/detach edges, bounce rejection, and blip
 *        immunity. Expected output: 5 Tests 0 Failures 0 Ignored.
 */
#include "unity.h"

#include "usb_host_manager_private.h"

static uhm_presence_t s_p;

void setUp(void)
{
    uhm_presence_init(&s_p, 1); /* boots with nothing attached */
}

void tearDown(void)
{
}

static void test_boot_states(void)
{
    uhm_presence_t p;

    uhm_presence_init(&p, 1);
    TEST_ASSERT_FALSE(p.present);
    uhm_presence_init(&p, 0); /* already plugged at power-on */
    TEST_ASSERT_TRUE(p.present);
}

static void test_stable_attach_edge(void)
{
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 0));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 0));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_ATTACH,
                          uhm_presence_sample(&s_p, 0));
    TEST_ASSERT_TRUE(s_p.present);
    /* settled: no repeats */
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 0));
}

static void test_stable_detach_edge(void)
{
    for (int i = 0; i < 3; i++)
    {
        uhm_presence_sample(&s_p, 0); /* attach */
    }

    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 1));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 1));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_DETACH,
                          uhm_presence_sample(&s_p, 1));
    TEST_ASSERT_FALSE(s_p.present);
}

static void test_bounce_never_edges(void)
{
    /* cable wiggle: alternating levels must never produce an edge */
    for (int i = 0; i < 20; i++)
    {
        TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE,
                              uhm_presence_sample(&s_p, i % 2));
    }

    TEST_ASSERT_FALSE(s_p.present);
}

static void test_blip_resets_the_count(void)
{
    /* two low samples, a high blip, then lows again: the counter must
       restart — edge only after 3 CONSECUTIVE agreeing samples */
    uhm_presence_sample(&s_p, 0);
    uhm_presence_sample(&s_p, 0);
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 1));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 0));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_NONE, uhm_presence_sample(&s_p, 0));
    TEST_ASSERT_EQUAL_INT(UHM_EDGE_ATTACH,
                          uhm_presence_sample(&s_p, 0));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_states);
    RUN_TEST(test_stable_attach_edge);
    RUN_TEST(test_stable_detach_edge);
    RUN_TEST(test_bounce_never_edges);
    RUN_TEST(test_blip_resets_the_count);
    UNITY_END();
}
