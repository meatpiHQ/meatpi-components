/**
 * @file test_main.c
 * @brief Host tests for interface_manager's pure arbitration rules
 *        (the legacy wireless-mode behaviors, generalized).
 *        Expected: 7 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "interface_manager_private.h"

static im_inputs_t s_in;
static im_target_t s_out;

void setUp(void)
{
    memset(&s_in, 0, sizeof(s_in));
    s_in.rule_sta_ble_handover = true;
    s_in.rule_ap_ble_exclusive = true;
}

void tearDown(void)
{
}

static void test_idle_nothing_suspended(void)
{
    /* apsta + ble enabled, nobody connected: everything advertises */
    s_in.mode_has_sta = true;
    s_in.mode_has_ap = true;
    s_in.ble_enabled = true;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_FALSE(s_out.suspend_sta);
    TEST_ASSERT_FALSE(s_out.suspend_ap);
    TEST_ASSERT_FALSE(s_out.suspend_ble);
}

static void test_ble_off_means_no_rules(void)
{
    s_in.mode_has_sta = true;
    s_in.mode_has_ap = true;
    s_in.ble_enabled = false;
    s_in.ap_has_clients = true; /* would trigger R2 if BLE were on */
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_FALSE(s_out.suspend_ble);
    TEST_ASSERT_FALSE(s_out.suspend_ap);
}

static void test_sta_ble_handover(void)
{
    /* the legacy sta+BLE mode: BLE client -> car -> STA yields */
    s_in.mode_has_sta = true;
    s_in.ble_enabled = true;
    s_in.ble_connected = true;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_TRUE(s_out.suspend_sta);
    TEST_ASSERT_FALSE(s_out.suspend_ble);

    /* back home: phone BLE off -> STA resumes */
    s_in.ble_connected = false;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_FALSE(s_out.suspend_sta);
}

static void test_ap_ble_ble_client_wins(void)
{
    s_in.mode_has_ap = true;
    s_in.ble_enabled = true;
    s_in.ble_connected = true;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_TRUE(s_out.suspend_ap);
    TEST_ASSERT_FALSE(s_out.suspend_ble);
}

static void test_ap_ble_ap_station_wins(void)
{
    s_in.mode_has_ap = true;
    s_in.ble_enabled = true;
    s_in.ap_has_clients = true;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_TRUE(s_out.suspend_ble);
    TEST_ASSERT_FALSE(s_out.suspend_ap);
}

static void test_tie_goes_to_ble(void)
{
    /* both connected during a race: the drive signal outranks */
    s_in.mode_has_ap = true;
    s_in.ble_enabled = true;
    s_in.ble_connected = true;
    s_in.ap_has_clients = true;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_TRUE(s_out.suspend_ap);
    TEST_ASSERT_FALSE(s_out.suspend_ble);
}

static void test_rule_toggles_disable(void)
{
    s_in.mode_has_sta = true;
    s_in.mode_has_ap = true;
    s_in.ble_enabled = true;
    s_in.ble_connected = true;
    s_in.rule_sta_ble_handover = false;
    s_in.rule_ap_ble_exclusive = false;
    im_policy_evaluate(&s_in, &s_out);
    TEST_ASSERT_FALSE(s_out.suspend_sta);
    TEST_ASSERT_FALSE(s_out.suspend_ap);
    TEST_ASSERT_FALSE(s_out.suspend_ble);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_nothing_suspended);
    RUN_TEST(test_ble_off_means_no_rules);
    RUN_TEST(test_sta_ble_handover);
    RUN_TEST(test_ap_ble_ble_client_wins);
    RUN_TEST(test_ap_ble_ap_station_wins);
    RUN_TEST(test_tie_goes_to_ble);
    RUN_TEST(test_rule_toggles_disable);
    UNITY_END();
}
