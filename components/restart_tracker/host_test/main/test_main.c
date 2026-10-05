/**
 * @file test_main.c
 * @brief Host test entry point for the restart_tracker core.
 */
#include "unity.h"

void test_random_memory_is_invalid_and_resets(void);
void test_boot_recording_and_counters(void);
void test_planned_restart_consumed_once(void);
void test_unexpected_reason_classification(void);
void test_history_ring_wraps(void);
void test_crc_detects_tamper(void);
void test_tuning_guard_clobber_is_harmless(void);
void test_invalid_time_stored_as_zero(void);

/* test_crash_core.c: the crash note's pure half */
void test_crash_layout(void);
void test_crash_random_memory_is_no_note(void);
void test_crash_head_crc_catches_a_flipped_bit(void);
void test_crash_tail_crc_catches_a_flipped_bit(void);
void test_crash_pending_lands_in_the_records_slot(void);
void test_crash_head_only_note_is_filed_incomplete(void);
void test_crash_no_pending_clears_the_slot(void);
void test_crash_foreign_store_clears_kept_and_files_pending(void);
void test_crash_fresh_history_clears_kept_and_files_pending(void);
void test_crash_lookup_never_returns_another_boot(void);
void test_crash_bad_kind_or_slot_is_refused(void);
void test_crash_summary_exception(void);
void test_crash_summary_abort_watchdog_and_head_only(void);
void test_crash_text_is_made_printable(void);
void test_crash_kind_names(void);

/* test_brake_core.c: the crash-loop brake's pure half */
void test_brake_layout_and_validity(void);
void test_brake_which_resets_are_crashes(void);
void test_brake_three_quick_crashes_park(void);
void test_brake_a_settled_run_ends_the_streak(void);
void test_brake_any_other_reset_ends_the_streak(void);
void test_brake_a_park_that_retries_keeps_the_streak(void);
void test_brake_a_retry_that_settles_is_healthy_again(void);
void test_brake_a_park_ended_from_outside_starts_over(void);
void test_brake_a_crash_inside_the_park_parks_bare(void);
void test_brake_safe_mode_is_not_braked(void);
void test_brake_retry_time(void);
void test_brake_report_budget(void);
void test_brake_a_long_loop_stays_parked(void);
void test_brake_count_outlives_the_psram_history(void);
void test_brake_record_fields_and_names(void);

/* test_report_core.c: the stored crash report's pure half */
void test_report_layout_and_validity(void);
void test_report_build_fields(void);
void test_report_identity(void);
void test_report_decide_first_same_and_other(void);
void test_report_decide_news_about_the_same_crash(void);
void test_report_a_crash_loop_costs_a_bounded_number_of_writes(void);
void test_report_text(void);
void test_report_text_of_what_was_not_recorded(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_random_memory_is_invalid_and_resets);
    RUN_TEST(test_boot_recording_and_counters);
    RUN_TEST(test_planned_restart_consumed_once);
    RUN_TEST(test_unexpected_reason_classification);
    RUN_TEST(test_history_ring_wraps);
    RUN_TEST(test_crc_detects_tamper);
    RUN_TEST(test_tuning_guard_clobber_is_harmless);
    RUN_TEST(test_invalid_time_stored_as_zero);
    RUN_TEST(test_crash_layout);
    RUN_TEST(test_crash_random_memory_is_no_note);
    RUN_TEST(test_crash_head_crc_catches_a_flipped_bit);
    RUN_TEST(test_crash_tail_crc_catches_a_flipped_bit);
    RUN_TEST(test_crash_pending_lands_in_the_records_slot);
    RUN_TEST(test_crash_head_only_note_is_filed_incomplete);
    RUN_TEST(test_crash_no_pending_clears_the_slot);
    RUN_TEST(test_crash_foreign_store_clears_kept_and_files_pending);
    RUN_TEST(test_crash_fresh_history_clears_kept_and_files_pending);
    RUN_TEST(test_crash_lookup_never_returns_another_boot);
    RUN_TEST(test_crash_bad_kind_or_slot_is_refused);
    RUN_TEST(test_crash_summary_exception);
    RUN_TEST(test_crash_summary_abort_watchdog_and_head_only);
    RUN_TEST(test_crash_text_is_made_printable);
    RUN_TEST(test_crash_kind_names);
    RUN_TEST(test_brake_layout_and_validity);
    RUN_TEST(test_brake_which_resets_are_crashes);
    RUN_TEST(test_brake_three_quick_crashes_park);
    RUN_TEST(test_brake_a_settled_run_ends_the_streak);
    RUN_TEST(test_brake_any_other_reset_ends_the_streak);
    RUN_TEST(test_brake_a_park_that_retries_keeps_the_streak);
    RUN_TEST(test_brake_a_retry_that_settles_is_healthy_again);
    RUN_TEST(test_brake_a_park_ended_from_outside_starts_over);
    RUN_TEST(test_brake_a_crash_inside_the_park_parks_bare);
    RUN_TEST(test_brake_safe_mode_is_not_braked);
    RUN_TEST(test_brake_retry_time);
    RUN_TEST(test_brake_report_budget);
    RUN_TEST(test_brake_a_long_loop_stays_parked);
    RUN_TEST(test_brake_count_outlives_the_psram_history);
    RUN_TEST(test_brake_record_fields_and_names);
    RUN_TEST(test_report_layout_and_validity);
    RUN_TEST(test_report_build_fields);
    RUN_TEST(test_report_identity);
    RUN_TEST(test_report_decide_first_same_and_other);
    RUN_TEST(test_report_decide_news_about_the_same_crash);
    RUN_TEST(test_report_a_crash_loop_costs_a_bounded_number_of_writes);
    RUN_TEST(test_report_text);
    RUN_TEST(test_report_text_of_what_was_not_recorded);
    UNITY_END();
}
