/**
 * @file test_main.c
 * @brief Host tests for websocket_manager's pure policy layer: channel
 *        parse defaults, the /ws/ namespace rule, cross-item uniqueness.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "websocket_manager.h"
#include "websocket_manager_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static esp_err_t validate(const char *json, char *err, size_t err_len)
{
    cJSON *arr = cJSON_Parse(json);
    esp_err_t r = wsm_validate_channels(arr, err, err_len);

    cJSON_Delete(arr);
    return r;
}

static void test_parse_defaults(void)
{
    cJSON *item = cJSON_Parse(
        "{\"name\":\"ws_obd\",\"path\":\"/ws/obd\"}");
    wsm_channel_cfg_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, wsm_parse_channel(item, &cfg));
    TEST_ASSERT_EQUAL(2, cfg.max_clients);  /* code default */
    TEST_ASSERT_FALSE(cfg.text_mode);       /* binary default */
    TEST_ASSERT_FALSE(cfg.enabled);
    cJSON_Delete(item);
}

static void test_text_mode_parsed(void)
{
    cJSON *item = cJSON_Parse(
        "{\"name\":\"ws_cli\",\"path\":\"/ws/cli\",\"mode\":\"text\"}");
    wsm_channel_cfg_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, wsm_parse_channel(item, &cfg));
    TEST_ASSERT_TRUE(cfg.text_mode);
    cJSON_Delete(item);
}

static void test_namespace_rule(void)
{
    wsm_channel_cfg_t cfg;

    /* /api and bare paths are other owners' namespaces */
    cJSON *bad1 = cJSON_Parse(
        "{\"name\":\"x\",\"path\":\"/api/ws\"}");
    cJSON *bad2 = cJSON_Parse(
        "{\"name\":\"x\",\"path\":\"/obd\"}");

    TEST_ASSERT_NOT_EQUAL(ESP_OK, wsm_parse_channel(bad1, &cfg));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, wsm_parse_channel(bad2, &cfg));
    cJSON_Delete(bad1);
    cJSON_Delete(bad2);
}

static void test_valid_set_and_duplicates(void)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"ws_obd\",\"path\":\"/ws/obd\",\"enabled\":true},"
         "{\"name\":\"ws_can\",\"path\":\"/ws/can\"}]", err, sizeof(err)));

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"path\":\"/ws/x\"},"
         "{\"name\":\"a\",\"path\":\"/ws/y\"}]", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "duplicate channel name"));

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"path\":\"/ws/x\"},"
         "{\"name\":\"b\",\"path\":\"/ws/x\"}]", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "duplicate path"));
}

static void test_max_clients_bounds(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"path\":\"/ws/x\",\"max_clients\":9}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "max_clients"));
}

/* ---- v1 -> v2 migration (the ws_log default channel) ------------------- */

static cJSON *settings_with(const char *channels_json)
{
    cJSON *settings = cJSON_CreateObject();

    cJSON_AddItemToObject(settings, "channels",
                          cJSON_Parse(channels_json));
    return settings;
}

static void test_migrate_v1_appends_ws_log(void)
{
    cJSON *settings = settings_with(
        "[{\"name\":\"ws_obd\",\"path\":\"/ws/obd\",\"enabled\":true}]");

    TEST_ASSERT_EQUAL(ESP_OK, wsm_migrate_channels(1, settings));

    cJSON *channels = cJSON_GetObjectItem(settings, "channels");

    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(channels));

    cJSON *added = cJSON_GetArrayItem(channels, 1);

    TEST_ASSERT_EQUAL_STRING("ws_log",
        cJSON_GetObjectItem(added, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("/ws/log",
        cJSON_GetObjectItem(added, "path")->valuestring);
    TEST_ASSERT_EQUAL_STRING("text",
        cJSON_GetObjectItem(added, "mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(added, "enabled")));

    /* the migrated array still validates */
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK,
                      wsm_validate_channels(channels, err, sizeof(err)));
    cJSON_Delete(settings);
}

static void test_migrate_skips_when_name_or_path_taken(void)
{
    cJSON *a = settings_with(
        "[{\"name\":\"ws_log\",\"path\":\"/ws/mylog\"}]");
    cJSON *b = settings_with(
        "[{\"name\":\"mine\",\"path\":\"/ws/log\"}]");

    TEST_ASSERT_EQUAL(ESP_OK, wsm_migrate_channels(1, a));
    TEST_ASSERT_EQUAL(ESP_OK, wsm_migrate_channels(1, b));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(
        cJSON_GetObjectItem(a, "channels")));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(
        cJSON_GetObjectItem(b, "channels")));
    cJSON_Delete(a);
    cJSON_Delete(b);
}

static void test_migrate_skips_full_table_and_v2(void)
{
    /* full table: MAX_CHANNELS entries already */
    char json[512] = "[";

    for (int i = 0; i < WEBSOCKET_MANAGER_MAX_CHANNELS; i++)
    {
        char item[80];

        snprintf(item, sizeof(item),
                 "%s{\"name\":\"c%d\",\"path\":\"/ws/c%d\"}",
                 i ? "," : "", i, i);
        strcat(json, item);
    }

    strcat(json, "]");

    cJSON *full = settings_with(json);

    TEST_ASSERT_EQUAL(ESP_OK, wsm_migrate_channels(1, full));
    TEST_ASSERT_EQUAL(WEBSOCKET_MANAGER_MAX_CHANNELS, cJSON_GetArraySize(
        cJSON_GetObjectItem(full, "channels")));
    cJSON_Delete(full);

    /* already v2: untouched */
    cJSON *v2 = settings_with(
        "[{\"name\":\"ws_obd\",\"path\":\"/ws/obd\"}]");

    TEST_ASSERT_EQUAL(ESP_OK, wsm_migrate_channels(2, v2));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(
        cJSON_GetObjectItem(v2, "channels")));
    cJSON_Delete(v2);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_defaults);
    RUN_TEST(test_text_mode_parsed);
    RUN_TEST(test_namespace_rule);
    RUN_TEST(test_valid_set_and_duplicates);
    RUN_TEST(test_max_clients_bounds);
    RUN_TEST(test_migrate_v1_appends_ws_log);
    RUN_TEST(test_migrate_skips_when_name_or_path_taken);
    RUN_TEST(test_migrate_skips_full_table_and_v2);
    UNITY_END();
}
