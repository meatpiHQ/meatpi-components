/**
 * @file test_main.c
 * @brief Host test entry point for http_server_manager match logic.
 */
#include "unity.h"

void test_normalize_strips_query_and_fragment(void);
void test_normalize_root_maps_to_index(void);
void test_normalize_rejects_traversal(void);
void test_normalize_rejects_overlong(void);
void test_resolve_exact_embedded_first_table_wins(void);
void test_resolve_exact_fs_path(void);
void test_resolve_prefix_joins_remainder(void);
void test_resolve_prefix_requires_remainder(void);
void test_resolve_second_table_reachable(void);
void test_resolve_no_match_is_null(void);
void test_mime_inference(void);
void test_auth_open_when_no_password(void);
void test_auth_missing_credentials_rejected(void);
void test_auth_basic_any_username(void);
void test_auth_basic_garbage_rejected(void);
void test_auth_bearer(void);
void test_auth_cookie(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normalize_strips_query_and_fragment);
    RUN_TEST(test_normalize_root_maps_to_index);
    RUN_TEST(test_normalize_rejects_traversal);
    RUN_TEST(test_normalize_rejects_overlong);
    RUN_TEST(test_resolve_exact_embedded_first_table_wins);
    RUN_TEST(test_resolve_exact_fs_path);
    RUN_TEST(test_resolve_prefix_joins_remainder);
    RUN_TEST(test_resolve_prefix_requires_remainder);
    RUN_TEST(test_resolve_second_table_reachable);
    RUN_TEST(test_resolve_no_match_is_null);
    RUN_TEST(test_mime_inference);
    RUN_TEST(test_auth_open_when_no_password);
    RUN_TEST(test_auth_missing_credentials_rejected);
    RUN_TEST(test_auth_basic_any_username);
    RUN_TEST(test_auth_basic_garbage_rejected);
    RUN_TEST(test_auth_bearer);
    RUN_TEST(test_auth_cookie);
    UNITY_END();
}
