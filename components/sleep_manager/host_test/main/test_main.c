/**
 * @file test_main.c
 * @brief Host tests for the pure sleep policy ladder: countdown +
 *        recovery, sleep entry, stable-wake reboot, band dips,
 *        periodic check-in gating, ms-clock wrap, the wake hold following
 *        wake_delay_ms; and the settings rules (v3 threshold resolver,
 *        v2 -> v3 and v3 -> v4 migration values).
 *        Plus the critical battery floor tracker (2026-10-01).
 *        Expected output: 13 Tests 0 Failures 0 Ignored.
 */
#include "unity.h"

#include "sleep_manager_private.h"

/* the legacy pair: sleep 13.1 V, wake 13.2 V, 1 s hold, 3 min delay */
static sm_cfg_t s_cfg;
static sm_policy_t s_p;

void setUp(void)
{
    s_cfg.sleep_v = 13.1f;
    s_cfg.wake_v = 13.2f;
    s_cfg.wake_hold_ms = 1000;
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
    /* engine off: 12.4 V, countdown starts */
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

/* v3: the stored pair resolves as given when the band is >= 0.1 V and is
   pulled up to sleep + 0.1 V (reported) when it is not */
static void test_resolve_thresholds(void)
{
    float s, w;

    TEST_ASSERT_FALSE(sm_resolve_thresholds(13100, 13300, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.1f, s);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.3f, w);
    /* exactly the minimum band is fine (the migrated default pair) */
    TEST_ASSERT_FALSE(sm_resolve_thresholds(13100, 13200, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.2f, w);
    /* equal: clamped */
    TEST_ASSERT_TRUE(sm_resolve_thresholds(12900, 12900, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 12.9f, s);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.0f, w);
    /* below sleep: clamped, sleep untouched */
    TEST_ASSERT_TRUE(sm_resolve_thresholds(14000, 12100, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 14.0f, s);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 14.1f, w);
    /* a wide band is the user's choice */
    TEST_ASSERT_FALSE(sm_resolve_thresholds(12500, 13800, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.8f, w);
}

/* v4: the wake hold follows the setting: a 5 s hold waits the full 5 s
   (and dips still cancel), a 100 ms hold wakes almost at once */
static void test_wake_hold_follows_setting(void)
{
    s_cfg.wake_hold_ms = 5000;
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_WAKE_PENDING, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 204900));
    TEST_ASSERT_EQUAL_INT(SM_ACT_WAKE_REBOOT,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 205000));

    s_cfg.wake_hold_ms = 100;
    sm_policy_init(&s_p);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200050));
    TEST_ASSERT_EQUAL_INT(SM_ACT_WAKE_REBOOT,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200100));
}

/* v3 -> v4: a document without wake_delay_ms gets the default; one that
   has it, or a current document, is left alone */
static void test_migrated_wake_delay_ms(void)
{
    TEST_ASSERT_EQUAL_INT(500, sm_migrated_wake_delay_ms(3, false));
    TEST_ASSERT_EQUAL_INT(500, sm_migrated_wake_delay_ms(1, false));
    TEST_ASSERT_EQUAL_INT(0, sm_migrated_wake_delay_ms(3, true));
    TEST_ASSERT_EQUAL_INT(0, sm_migrated_wake_delay_ms(4, false));
}

/* v2 -> v3: every historical document gets the band it had (sleep + 100),
   kept inside the new field's range */
static void test_migrated_wake_mv(void)
{
    TEST_ASSERT_EQUAL_INT(13200, sm_migrated_wake_mv(13100, 12100, 15000));
    TEST_ASSERT_EQUAL_INT(12100, sm_migrated_wake_mv(12000, 12100, 15000));
    TEST_ASSERT_EQUAL_INT(14100, sm_migrated_wake_mv(14000, 12100, 15000));
    /* a v1 document is clamped to the v2 ranges first; the rule still
       stays inside the field's range if it were not */
    TEST_ASSERT_EQUAL_INT(12100, sm_migrated_wake_mv(11000, 12100, 15000));
    TEST_ASSERT_EQUAL_INT(15000, sm_migrated_wake_mv(15500, 12100, 15000));
}

/* the critical floor: 120 s under 11.90 V trips once, then re-arms */
static void test_critical_floor_trips_after_delay(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 0));         /* arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 60000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 119000));
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.7f, 120000));     /* trip */
    /* one trip per arming; the next reading under the floor re-arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 121000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 240000));
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.7f, 241000));
    /* a healthy battery never arms it */
    sm_critical_init(&c);
    for (uint32_t t = 0; t < 600000; t += 1000)
    {
        TEST_ASSERT_FALSE(sm_critical_eval(&c, 12.4f, t));
    }
}

/* recovery above the hysteresis band resets the timer; the band itself
   neither arms nor resets it */
static void test_critical_floor_resets_on_recovery(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    sm_critical_eval(&c, 11.7f, 0);                            /* arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 60000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 12.0f, 61000));     /* reset */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 62000));     /* re-arm */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 181000));    /* 119 s */
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.7f, 182000));     /* 120 s */
    /* 11.92 V sits in the band: keeps an armed timer, arms nothing */
    sm_critical_init(&c);
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 0));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 200000));   /* never armed */
    sm_critical_eval(&c, 11.8f, 300000);                       /* arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 400000));   /* holds */
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.92f, 420000));    /* trips */
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
    RUN_TEST(test_resolve_thresholds);
    RUN_TEST(test_migrated_wake_mv);
    RUN_TEST(test_wake_hold_follows_setting);
    RUN_TEST(test_migrated_wake_delay_ms);
    RUN_TEST(test_critical_floor_trips_after_delay);
    RUN_TEST(test_critical_floor_resets_on_recovery);
    UNITY_END();
}
