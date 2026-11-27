/**
 * @file test_main.c
 * @brief Host suite for se_script_name_ok: the script-name guard that
 *        protects the /data/scripts load path (script_engine.c) and the
 *        script.run event action (script_engine_events.c) from path
 *        traversal / injection. Security-relevant: a bad name reaches a
 *        filesystem open, so this must reject anything that isn't a plain
 *        [A-Za-z0-9_.-] filename of <=40 chars with no "..".
 */
#include "unity.h"

bool se_script_name_ok(const char *name);

/* test_obd.c: obd.* conversation core suite */
void test_obd_claim_release_cycle(void);
void test_obd_claim_same_addr_extends(void);
void test_obd_claim_other_addr_busy(void);
void test_obd_claim_port_failure(void);
void test_obd_release_idempotent(void);
void test_obd_autorelease_after_run(void);
void test_obd_request_requires_claim(void);
void test_obd_request_plumbs_outcome(void);
void test_obd_request_negative_response(void);
void test_obd_isotp_requires_claim(void);
void test_obd_isotp_under_claim(void);
void test_obd_bad_args(void);

/* test_reflash.c: se_obd_transfer_file suite */
void test_xfer_requires_claim(void);
void test_xfer_blocks_and_bsc(void);
void test_xfer_bsc_wraps(void);
void test_xfer_crc32_check_value(void);
void test_xfer_negative_response_stops(void);
void test_xfer_read_failure(void);
void test_xfer_bad_args(void);

/* test_doc.c: the reference + example gallery tables */
void test_doc_bindings_table_is_consistent(void);
void test_doc_reference_json_shape(void);
void test_examples_are_valid_scripts(void);
void test_examples_only_use_documented_bindings(void);
void test_examples_json_lists_without_sources(void);

/* test_flash.c: ISO-TP frames, the flash lock, the checksums */
void test_tp_single_frame_padded(void);
void test_tp_extended_address_single(void);
void test_tp_multi_frame_segmentation(void);
void test_tp_extended_address_round_trip(void);
void test_tp_sequence_error_and_overflow(void);
void test_tp_flow_control_parse(void);
void test_tp_rejects_bad_lengths(void);
void test_tp_block_size_flow_control(void);
void test_lock_lifecycle(void);
void test_lock_tx_gate(void);
void test_lock_http_gate(void);
void test_crc_catalogue(void);
void test_adler32_and_sum(void);

void test_gpio_denylist(void);
void test_gpio_can_alias_needs_stop(void);
void test_gpio_only_can_tx_is_driven(void);
void test_pwm_span(void);
void test_gpio_touch_release(void);
void test_pwm_default_idle(void);

void test_accepts_plain_names(void)
{
    TEST_ASSERT_TRUE(se_script_name_ok("vin"));
    TEST_ASSERT_TRUE(se_script_name_ok("read_vin"));
    TEST_ASSERT_TRUE(se_script_name_ok("read-vin"));
    TEST_ASSERT_TRUE(se_script_name_ok("clear.dtc"));
    TEST_ASSERT_TRUE(se_script_name_ok("job2"));
    TEST_ASSERT_TRUE(se_script_name_ok("A"));
}

void test_rejects_null_and_empty(void)
{
    TEST_ASSERT_FALSE(se_script_name_ok(NULL));
    TEST_ASSERT_FALSE(se_script_name_ok(""));
}

void test_rejects_path_traversal(void)
{
    TEST_ASSERT_FALSE(se_script_name_ok(".."));
    TEST_ASSERT_FALSE(se_script_name_ok("../secrets"));
    TEST_ASSERT_FALSE(se_script_name_ok("a..b"));      /* ".." anywhere */
    TEST_ASSERT_FALSE(se_script_name_ok("ok..still"));
}

void test_rejects_path_separators(void)
{
    TEST_ASSERT_FALSE(se_script_name_ok("sub/script"));
    TEST_ASSERT_FALSE(se_script_name_ok("/etc/passwd"));
    TEST_ASSERT_FALSE(se_script_name_ok("a\\b"));       /* backslash */
}

void test_rejects_injection_chars(void)
{
    TEST_ASSERT_FALSE(se_script_name_ok("a b"));        /* space */
    TEST_ASSERT_FALSE(se_script_name_ok("a\"b"));       /* quote */
    TEST_ASSERT_FALSE(se_script_name_ok("a;b"));        /* semicolon */
    TEST_ASSERT_FALSE(se_script_name_ok("a$b"));
    TEST_ASSERT_FALSE(se_script_name_ok("a\tb"));       /* tab */
    TEST_ASSERT_FALSE(se_script_name_ok("na\xC3\xA9")); /* non-ASCII */
}

void test_rejects_overlong(void)
{
    char n41[42];
    for (int i = 0; i < 41; i++)
    {
        n41[i] = 'a';
    }
    n41[41] = '\0';
    TEST_ASSERT_FALSE(se_script_name_ok(n41));          /* 41 > 40 */

    char n40[41];
    for (int i = 0; i < 40; i++)
    {
        n40[i] = 'a';
    }
    n40[40] = '\0';
    TEST_ASSERT_TRUE(se_script_name_ok(n40));           /* 40 is the cap */
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_accepts_plain_names);
    RUN_TEST(test_rejects_null_and_empty);
    RUN_TEST(test_rejects_path_traversal);
    RUN_TEST(test_rejects_path_separators);
    RUN_TEST(test_rejects_injection_chars);
    RUN_TEST(test_rejects_overlong);

    /* obd.* conversation core (test_obd.c) */
    RUN_TEST(test_obd_claim_release_cycle);
    RUN_TEST(test_obd_claim_same_addr_extends);
    RUN_TEST(test_obd_claim_other_addr_busy);
    RUN_TEST(test_obd_claim_port_failure);
    RUN_TEST(test_obd_release_idempotent);
    RUN_TEST(test_obd_autorelease_after_run);
    RUN_TEST(test_obd_request_requires_claim);
    RUN_TEST(test_obd_request_plumbs_outcome);
    RUN_TEST(test_obd_request_negative_response);
    RUN_TEST(test_obd_isotp_requires_claim);
    RUN_TEST(test_obd_isotp_under_claim);
    RUN_TEST(test_obd_bad_args);

    /* reflash TransferData streamer (test_reflash.c) */
    RUN_TEST(test_xfer_requires_claim);
    RUN_TEST(test_xfer_blocks_and_bsc);
    RUN_TEST(test_xfer_bsc_wraps);
    RUN_TEST(test_xfer_crc32_check_value);
    RUN_TEST(test_xfer_negative_response_stops);
    RUN_TEST(test_xfer_read_failure);
    RUN_TEST(test_xfer_bad_args);

    /* the scripting reference + examples (test_doc.c) */
    RUN_TEST(test_doc_bindings_table_is_consistent);
    RUN_TEST(test_doc_reference_json_shape);
    RUN_TEST(test_examples_are_valid_scripts);
    RUN_TEST(test_examples_only_use_documented_bindings);
    RUN_TEST(test_examples_json_lists_without_sources);

    RUN_TEST(test_gpio_denylist);
    RUN_TEST(test_gpio_can_alias_needs_stop);
    RUN_TEST(test_gpio_only_can_tx_is_driven);
    RUN_TEST(test_pwm_span);
    RUN_TEST(test_gpio_touch_release);
    RUN_TEST(test_pwm_default_idle);

    RUN_TEST(test_tp_single_frame_padded);
    RUN_TEST(test_tp_extended_address_single);
    RUN_TEST(test_tp_multi_frame_segmentation);
    RUN_TEST(test_tp_extended_address_round_trip);
    RUN_TEST(test_tp_sequence_error_and_overflow);
    RUN_TEST(test_tp_flow_control_parse);
    RUN_TEST(test_tp_rejects_bad_lengths);
    RUN_TEST(test_tp_block_size_flow_control);
    RUN_TEST(test_lock_lifecycle);
    RUN_TEST(test_lock_tx_gate);
    RUN_TEST(test_lock_http_gate);
    RUN_TEST(test_crc_catalogue);
    RUN_TEST(test_adler32_and_sum);

    UNITY_END();
}
