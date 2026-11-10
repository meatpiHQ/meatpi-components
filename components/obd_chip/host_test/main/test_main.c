/**
 * @file test_main.c
 * @brief Host test entry point for the obd_chip pure parsing/framing logic
 *        and (test_request.c) the request engine against a fake chip.
 */
#include "unity.h"

void test_simple_response(void);
void test_echo_off_response(void);
void test_prompt_split_across_chunks(void);
void test_bytes_after_prompt_not_consumed(void);
void test_unsolicited_noise_interleaves_into_window(void);
void test_long_real_car_response_all_splits(void);
void test_overflow_is_flagged_not_fatal(void);
void test_chip_error_classification(void);
void test_monitor_command_classification(void);
void test_fw_iterator_and_end_marker(void);
void test_fw_version_policy(void);
void test_eeprom_guard(void);
void test_protocol_switch_classification(void);
void test_reset_word_classification(void);
void test_extract_skips_reset_junk(void);
void test_pp_parse(void);
void test_stslcs_parse_full_native(void);
void test_stslcs_parse_elm327_and_armed_wake(void);
void test_stslcs_provision_policy(void);
void test_stslcs_ignores_garbage(void);
void test_stslcs_midline_gt_is_not_the_prompt(void);
void test_sleep_word_ack(void);
void test_ppsw_parse(void);
void test_request_switch_goes_out_behind_acknowledged_m0(void);
void test_request_switch_not_sent_when_m0_answers_what(void);
void test_request_switch_not_sent_when_m0_times_out(void);
void test_request_m0_must_be_ok_and_nothing_else(void);
void test_request_bus_requests_held_while_a_switch_is_owed(void);
void test_request_owed_switch_ends_with_the_echo_on(void);
void test_request_switch_inside_the_callers_transaction(void);
void test_request_no_hold_no_switch(void);
void test_claim_exclusive_holder_claims_again(void);
void test_request_bringup_clears_what_is_owed(void);
void test_bus_request_classification(void);
void test_save_lost_answer_is_not_written_twice(void);
void test_save_refused_word_waits_for_the_next_start(void);
void test_save_not_sent_when_the_chip_holds_it(void);
void test_save_writes_once_when_the_chip_holds_another(void);
void test_save_blocked_by_a_monitor_spends_nothing(void);
void test_save_refuses_what_is_not_a_protocol(void);
void test_dpn_parse(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simple_response);
    RUN_TEST(test_echo_off_response);
    RUN_TEST(test_prompt_split_across_chunks);
    RUN_TEST(test_bytes_after_prompt_not_consumed);
    RUN_TEST(test_unsolicited_noise_interleaves_into_window);
    RUN_TEST(test_long_real_car_response_all_splits);
    RUN_TEST(test_overflow_is_flagged_not_fatal);
    RUN_TEST(test_chip_error_classification);
    RUN_TEST(test_monitor_command_classification);
    RUN_TEST(test_fw_iterator_and_end_marker);
    RUN_TEST(test_fw_version_policy);
    RUN_TEST(test_eeprom_guard);
    RUN_TEST(test_protocol_switch_classification);
    RUN_TEST(test_reset_word_classification);
    RUN_TEST(test_extract_skips_reset_junk);
    RUN_TEST(test_pp_parse);
    RUN_TEST(test_stslcs_parse_full_native);
    RUN_TEST(test_stslcs_parse_elm327_and_armed_wake);
    RUN_TEST(test_stslcs_provision_policy);
    RUN_TEST(test_stslcs_ignores_garbage);
    RUN_TEST(test_stslcs_midline_gt_is_not_the_prompt);
    RUN_TEST(test_sleep_word_ack);
    RUN_TEST(test_ppsw_parse);
    RUN_TEST(test_request_switch_goes_out_behind_acknowledged_m0);
    RUN_TEST(test_request_switch_not_sent_when_m0_answers_what);
    RUN_TEST(test_request_switch_not_sent_when_m0_times_out);
    RUN_TEST(test_request_m0_must_be_ok_and_nothing_else);
    RUN_TEST(test_request_bus_requests_held_while_a_switch_is_owed);
    RUN_TEST(test_request_owed_switch_ends_with_the_echo_on);
    RUN_TEST(test_request_switch_inside_the_callers_transaction);
    RUN_TEST(test_request_no_hold_no_switch);
    RUN_TEST(test_claim_exclusive_holder_claims_again);
    RUN_TEST(test_request_bringup_clears_what_is_owed);
    RUN_TEST(test_bus_request_classification);
    RUN_TEST(test_save_lost_answer_is_not_written_twice);
    RUN_TEST(test_save_refused_word_waits_for_the_next_start);
    RUN_TEST(test_save_not_sent_when_the_chip_holds_it);
    RUN_TEST(test_save_writes_once_when_the_chip_holds_another);
    RUN_TEST(test_save_blocked_by_a_monitor_spends_nothing);
    RUN_TEST(test_save_refuses_what_is_not_a_protocol);
    RUN_TEST(test_dpn_parse);
    UNITY_END();
}
