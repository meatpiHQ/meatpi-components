/**
 * @file test_main.c
 * @brief Host tests for the pure sleep policy ladder: countdown +
 *        recovery, sleep entry, stable-wake reboot, band dips,
 *        periodic check-in gating, and ms-clock wrap.
 *        Expected output: 7 Tests 0 Failures 0 Ignored.
 */
#include "unity.h"

#include "sleep_manager_private.h"

/* the shipped defaults: sleep 13.1 V, wake 13.2 V, 3 min delay */
static sm_cfg_t s_cfg;
static sm_policy_t s_p;

void setUp(void)
{
    s_cfg.sleep_v = 13.1f;
    s_cfg.wake_v = 13.2f;
    s_cfg.delay_ms = 180000;
    s_cfg.periodic = false;
    s_cfg.interval_ms = 1800000;
    sm_policy_init(&s_p);
}

void tearDown(void)
{
}

static void test_healthy_battery_stays_normal(void)
{
    for (uint32_t t = 0; t < 600000; t += 1000)
    {
        TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                              sm_policy_eval(&s_p, &s_cfg, 14.2f, t));
        TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_NORMAL, s_p.state);
    }
}

static void test_countdown_then_sleep(void)
{
    /* engine off: 12.4 V — countdown starts */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 1000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    /* still counting at +179 s */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 180000));
    /* expiry → sleep */
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 181001));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
}

static void test_recovery_cancels_countdown(void)
{
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 1000);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    /* alternator back: above wake_v cancels */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 13.8f, 60000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_NORMAL, s_p.state);
    /* the 13.1..13.2 band does NOT cancel (hysteresis) */
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 61000);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 13.15f, 62000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
}

static void test_stable_recovery_reboots(void)
{
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
    /* engine started */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_WAKE_PENDING, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_WAKE_REBOOT,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 201500));
}

static void test_wake_dip_goes_back_to_sleep(void)
{
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000); /* WAKE_PENDING */
    /* headlights flash / spike gone: dip before the hold expires */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.5f, 200500));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
}

static void test_periodic_wake_gated_by_critical(void)
{
    s_cfg.periodic = true;
    s_cfg.interval_ms = 60000;
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001); /* SLEEPING, t+60s */
    /* healthy-ish battery: periodic fires after the interval */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 239000));
    TEST_ASSERT_EQUAL_INT(SM_ACT_PERIODIC_WAKE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 241000));
    /* below the critical floor it must NEVER fire (don't kill the
       battery for a check-in) */
    sm_policy_init(&s_p);
    sm_policy_eval(&s_p, &s_cfg, 11.5f, 0);
    sm_policy_eval(&s_p, &s_cfg, 11.5f, 180001);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 11.5f, 900000));
}

static void test_clock_wrap_safe(void)
{
    /* countdown straddling the u32 ms wrap */
    uint32_t t0 = 0xFFFFFFFFu - 60000u;

    sm_policy_eval(&s_p, &s_cfg, 12.4f, t0);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f,
                                         t0 + 100000u)); /* wrapped */
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f,
                                         t0 + 181000u));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_healthy_battery_stays_normal);
    RUN_TEST(test_countdown_then_sleep);
    RUN_TEST(test_recovery_cancels_countdown);
    RUN_TEST(test_stable_recovery_reboots);
    RUN_TEST(test_wake_dip_goes_back_to_sleep);
    RUN_TEST(test_periodic_wake_gated_by_critical);
    RUN_TEST(test_clock_wrap_safe);
    UNITY_END();
}
