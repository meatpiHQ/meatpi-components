/**
 * @file test_util.c
 * @brief Host tests for the pure api_http utility layer: password
 *        redaction/unredaction, settings route parsing, level mapping.
 */
#include <string.h>

#include "esp_log.h"
#include "unity.h"

#include "api_http_private.h"

/* ---- redaction ----------------------------------------------------------------- */

static void test_redact_passwords(void)
{
    cJSON *obj = cJSON_Parse(
        "{\"sta_ssid\":\"Home\",\"sta_password\":\"secret\","
        "\"fallback1_password\":\"other\",\"ap_channel\":6,"
        "\"password_hint\":\"keepme\"}");

    api_util_redact(obj);

    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "sta_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "fallback1_password")->valuestring);
    /* only secret-suffixed keys are redacted */
    TEST_ASSERT_EQUAL_STRING("Home", cJSON_GetObjectItem(obj,
                                 "sta_ssid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("keepme", cJSON_GetObjectItem(obj,
                                 "password_hint")->valuestring);
    TEST_ASSERT_EQUAL(6, cJSON_GetObjectItem(obj, "ap_channel")->valueint);
    cJSON_Delete(obj);
}

static void test_redact_wireguard_keys(void)
{
    /* the 2026-07-07 suffixes (vpn_manager): private/preshared keys are
       secrets, the peer's PUBLIC key is display data */
    cJSON *obj = cJSON_Parse(
        "{\"private_key\":\"wg-priv\",\"preshared_key\":\"wg-psk\","
        "\"peer_public_key\":\"wg-pub\",\"endpoint\":\"vpn.host\"}");

    api_util_redact(obj);

    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "private_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(obj,
                                 "preshared_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("wg-pub", cJSON_GetObjectItem(obj,
                                 "peer_public_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("vpn.host", cJSON_GetObjectItem(obj,
                                 "endpoint")->valuestring);
    cJSON_Delete(obj);
}

static void test_unredact_keeps_stored_on_empty(void)
{
    cJSON *stored = cJSON_Parse(
        "{\"sta_password\":\"stored-secret\",\"ap_password\":\"ap-secret\"}");
    cJSON *in = cJSON_Parse(
        "{\"sta_password\":\"\",\"ap_password\":\"new-pass\"}");

    api_util_unredact(in, stored);

    /* "" means keep the stored value; a real value passes through */
    TEST_ASSERT_EQUAL_STRING("stored-secret",
        cJSON_GetObjectItem(in, "sta_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("new-pass",
        cJSON_GetObjectItem(in, "ap_password")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

static void test_unredact_no_stored_value(void)
{
    cJSON *stored = cJSON_Parse("{}");
    cJSON *in = cJSON_Parse("{\"sta_password\":\"\"}");

    api_util_unredact(in, stored);

    /* nothing stored: the empty string stands (validates as empty) */
    TEST_ASSERT_EQUAL_STRING("",
        cJSON_GetObjectItem(in, "sta_password")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

/* ---- settings route parsing ------------------------------------------------------ */

static void test_path_plain_name(void)
{
    char name[40];
    bool is_schema = true;

    TEST_ASSERT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/wifi_manager", name, sizeof(name), &is_schema));
    TEST_ASSERT_EQUAL_STRING("wifi_manager", name);
    TEST_ASSERT_FALSE(is_schema);
}

static void test_path_schema(void)
{
    char name[40];
    bool is_schema = false;

    TEST_ASSERT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/log_manager/schema", name, sizeof(name), &is_schema));
    TEST_ASSERT_EQUAL_STRING("log_manager", name);
    TEST_ASSERT_TRUE(is_schema);
}

static void test_path_rejects_bad(void)
{
    char name[40];
    bool is_schema;

    /* empty name */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/", name, sizeof(name), &is_schema));
    /* extra segment */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/a/b", name, sizeof(name), &is_schema));
    /* bare "/schema" (empty name + suffix) */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings//schema", name, sizeof(name), &is_schema));
    /* wrong prefix */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/wifi/status", name, sizeof(name), &is_schema));
    /* name overflow */
    char tiny[4];

    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_settings_path(
        "/api/settings/wifi_manager", tiny, sizeof(tiny), &is_schema));
}

/* ---- level mapping ---------------------------------------------------------------- */

static void test_level_mapping(void)
{
    int lvl = -1;

    TEST_ASSERT_EQUAL(ESP_OK, api_util_level_from_str("debug", &lvl));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, lvl);
    TEST_ASSERT_EQUAL(ESP_OK, api_util_level_from_str("none", &lvl));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, lvl);
    TEST_ASSERT_EQUAL(ESP_OK, api_util_level_from_str("verbose", &lvl));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, lvl);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_level_from_str("loud", &lvl));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, api_util_level_from_str(NULL, &lvl));
}

void run_util_tests(void)
{
    RUN_TEST(test_redact_passwords);
    RUN_TEST(test_redact_wireguard_keys);
    RUN_TEST(test_unredact_keeps_stored_on_empty);
    RUN_TEST(test_unredact_no_stored_value);
    RUN_TEST(test_path_plain_name);
    RUN_TEST(test_path_schema);
    RUN_TEST(test_path_rejects_bad);
    RUN_TEST(test_level_mapping);
}
