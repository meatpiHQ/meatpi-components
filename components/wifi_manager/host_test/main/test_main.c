/**
 * @file test_main.c
 * @brief Host test entry point for the wifi_manager selection module.
 */
#include "unity.h"

void test_scan_prefers_primary(void);
void test_scan_falls_back_in_priority_order(void);
void test_scan_skips_failed_candidate(void);
void test_scan_all_failed_keeps_trying_round_robin(void);
void test_scan_clean_alternative_always_first(void);
void test_scan_nothing_present_returns_none(void);
void test_deprioritised_after_threshold_and_fade(void);
void test_faded_streak_starts_over(void);
void test_success_clears_failures(void);
void test_deprioritise_at_once(void);
void test_sequential_rotates_and_wraps(void);
void test_sequential_skips_failed(void);
void test_sequential_all_failed_rotates_anyway(void);
void test_sequential_first_pick_is_primary(void);
void test_single_network_keeps_trying(void);
void test_roam_better_available(void);
void test_duplicate_ssid_entries_are_independent(void);
void test_roam_never_to_same_ssid(void);
void test_parse_ap_ipv4(void);
void test_parse_ipv4_plain(void);
void test_default_hostname(void);
void test_netmask_valid(void);
void test_backoff_curve(void);
void test_ap_client_pause_policy(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan_prefers_primary);
    RUN_TEST(test_scan_falls_back_in_priority_order);
    RUN_TEST(test_scan_skips_failed_candidate);
    RUN_TEST(test_scan_all_failed_keeps_trying_round_robin);
    RUN_TEST(test_scan_clean_alternative_always_first);
    RUN_TEST(test_scan_nothing_present_returns_none);
    RUN_TEST(test_deprioritised_after_threshold_and_fade);
    RUN_TEST(test_faded_streak_starts_over);
    RUN_TEST(test_success_clears_failures);
    RUN_TEST(test_deprioritise_at_once);
    RUN_TEST(test_sequential_rotates_and_wraps);
    RUN_TEST(test_sequential_skips_failed);
    RUN_TEST(test_sequential_all_failed_rotates_anyway);
    RUN_TEST(test_sequential_first_pick_is_primary);
    RUN_TEST(test_single_network_keeps_trying);
    RUN_TEST(test_roam_better_available);
    RUN_TEST(test_duplicate_ssid_entries_are_independent);
    RUN_TEST(test_roam_never_to_same_ssid);
    RUN_TEST(test_parse_ap_ipv4);
    RUN_TEST(test_parse_ipv4_plain);
    RUN_TEST(test_default_hostname);
    RUN_TEST(test_netmask_valid);
    RUN_TEST(test_backoff_curve);
    RUN_TEST(test_ap_client_pause_policy);
    UNITY_END();
}
