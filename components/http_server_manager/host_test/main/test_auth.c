/**
 * @file test_auth.c
 * @brief Host tests for the pure admin-password check (2026-07-19):
 *        Basic (any username), Bearer, the wican_auth cookie, and the
 *        fail-closed corners.
 */
#include "unity.h"

#include "http_server_manager_private.h"

/* base64("user:secret99") = dXNlcjpzZWNyZXQ5OQ== */
#define BASIC_GOOD "Basic dXNlcjpzZWNyZXQ5OQ=="
/* base64(":secret99") = OnNlY3JldDk5 (empty username) */
#define BASIC_NOUSER "Basic OnNlY3JldDk5"
/* base64("user:wrong") = dXNlcjp3cm9uZw== */
#define BASIC_WRONG "Basic dXNlcjp3cm9uZw=="

void test_auth_open_when_no_password(void)
{
    TEST_ASSERT_TRUE(hsm_auth_check(NULL, NULL, ""));
    TEST_ASSERT_TRUE(hsm_auth_check(NULL, NULL, NULL));
    TEST_ASSERT_TRUE(hsm_auth_check(BASIC_WRONG, NULL, ""));
}

void test_auth_missing_credentials_rejected(void)
{
    TEST_ASSERT_FALSE(hsm_auth_check(NULL, NULL, "secret99"));
    TEST_ASSERT_FALSE(hsm_auth_check("", "", "secret99"));
}

void test_auth_basic_any_username(void)
{
    TEST_ASSERT_TRUE(hsm_auth_check(BASIC_GOOD, NULL, "secret99"));
    TEST_ASSERT_TRUE(hsm_auth_check(BASIC_NOUSER, NULL, "secret99"));
    TEST_ASSERT_FALSE(hsm_auth_check(BASIC_WRONG, NULL, "secret99"));
}

void test_auth_basic_garbage_rejected(void)
{
    TEST_ASSERT_FALSE(hsm_auth_check("Basic !!!not-base64!!!", NULL,
                                     "secret99"));
    /* no colon in the decoded credentials */
    TEST_ASSERT_FALSE(hsm_auth_check("Basic c2VjcmV0OTk=", NULL,
                                     "secret99")); /* "secret99" alone */
}

void test_auth_bearer(void)
{
    TEST_ASSERT_TRUE(hsm_auth_check("Bearer secret99", NULL, "secret99"));
    TEST_ASSERT_FALSE(hsm_auth_check("Bearer nope", NULL, "secret99"));
    /* prefix must not be enough (flat compare includes lengths) */
    TEST_ASSERT_FALSE(hsm_auth_check("Bearer secret9", NULL, "secret99"));
    TEST_ASSERT_FALSE(hsm_auth_check("Bearer secret999", NULL,
                                     "secret99"));
}

void test_auth_cookie(void)
{
    TEST_ASSERT_TRUE(hsm_auth_check(NULL, "wican_auth=secret99",
                                    "secret99"));
    TEST_ASSERT_TRUE(hsm_auth_check(NULL,
                                    "theme=dark; wican_auth=secret99; x=1",
                                    "secret99"));
    TEST_ASSERT_FALSE(hsm_auth_check(NULL, "wican_auth=nope", "secret99"));
    /* substring cookie names must not match */
    TEST_ASSERT_FALSE(hsm_auth_check(NULL, "xwican_auth=secret99",
                                     "secret99"));
}
