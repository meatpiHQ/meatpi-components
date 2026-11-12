/**
 * @file test_main.c
 * @brief Host test entry point for dev_status_manager pure helpers.
 */
#include "unity.h"

void test_bit_names_map_known_bits(void);
void test_bit_names_unknown(void);
void test_uptime_formats_hms(void);
void test_uptime_formats_days(void);
void test_uptime_truncation_and_errors(void);
void test_usb_kind_spellings(void);
void test_fault_first_occurrence_is_counted_and_written(void);
void test_fault_recurrence_is_written_once_per_boot(void);
void test_fault_count_stops_at_the_cap(void);
void test_fault_that_recurs_every_boot_stops_writing_at_the_cap(void);
void test_fault_one_short_of_the_cap_is_still_written(void);
void test_fault_count_of_an_earlier_build_is_kept(void);
void test_fault_capped_says_this_many_or_more(void);
void test_fault_record_layout_is_the_stored_one(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bit_names_map_known_bits);
    RUN_TEST(test_bit_names_unknown);
    RUN_TEST(test_uptime_formats_hms);
    RUN_TEST(test_uptime_formats_days);
    RUN_TEST(test_uptime_truncation_and_errors);
    RUN_TEST(test_usb_kind_spellings);
    RUN_TEST(test_fault_first_occurrence_is_counted_and_written);
    RUN_TEST(test_fault_recurrence_is_written_once_per_boot);
    RUN_TEST(test_fault_count_stops_at_the_cap);
    RUN_TEST(test_fault_that_recurs_every_boot_stops_writing_at_the_cap);
    RUN_TEST(test_fault_one_short_of_the_cap_is_still_written);
    RUN_TEST(test_fault_count_of_an_earlier_build_is_kept);
    RUN_TEST(test_fault_capped_says_this_many_or_more);
    RUN_TEST(test_fault_record_layout_is_the_stored_one);
    UNITY_END();
}
