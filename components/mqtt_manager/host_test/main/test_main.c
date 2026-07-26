/**
 * @file test_main.c
 * @brief Host tests for mqtt_manager's pure logic: MQTT-spec topic-filter
 *        matching (+/#, $-topics, malformed filters), broker-URL
 *        validation, and the async-ring item codec.
 *        Expected: 11 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "mqtt_manager_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_exact_match(void)
{
    TEST_ASSERT_TRUE(mm_topic_matches("wican/abc/status",
                                      "wican/abc/status"));
    TEST_ASSERT_FALSE(mm_topic_matches("wican/abc/status",
                                       "wican/abc/other"));
    TEST_ASSERT_FALSE(mm_topic_matches("wican/abc", "wican/abc/status"));
    TEST_ASSERT_FALSE(mm_topic_matches("wican/abc/status", "wican/abc"));
}

static void test_plus_matches_one_level(void)
{
    TEST_ASSERT_TRUE(mm_topic_matches("wican/+/status",
                                      "wican/abc/status"));
    TEST_ASSERT_FALSE(mm_topic_matches("wican/+/status",
                                       "wican/a/b/status"));
    TEST_ASSERT_TRUE(mm_topic_matches("+/+/+", "a/b/c"));
    /* '+' matches an EMPTY level too (MQTT spec) */
    TEST_ASSERT_TRUE(mm_topic_matches("a/+/c", "a//c"));
}

static void test_hash_matches_rest(void)
{
    TEST_ASSERT_TRUE(mm_topic_matches("wican/#", "wican/abc/can/rx"));
    TEST_ASSERT_TRUE(mm_topic_matches("#", "anything/at/all"));
    /* "sport/#" also matches the parent "sport" */
    TEST_ASSERT_TRUE(mm_topic_matches("sport/#", "sport"));
    TEST_ASSERT_FALSE(mm_topic_matches("sport/#", "sports"));
}

static void test_dollar_topics_hidden_from_wildcards(void)
{
    TEST_ASSERT_FALSE(mm_topic_matches("#", "$SYS/broker/load"));
    TEST_ASSERT_FALSE(mm_topic_matches("+/broker/load",
                                       "$SYS/broker/load"));
    /* explicit $ filters still work */
    TEST_ASSERT_TRUE(mm_topic_matches("$SYS/broker/load",
                                      "$SYS/broker/load"));
    TEST_ASSERT_TRUE(mm_topic_matches("$SYS/#", "$SYS/broker/load"));
}

static void test_malformed_filters_match_nothing(void)
{
    TEST_ASSERT_FALSE(mm_topic_matches("a/#/b", "a/x/b"));
    TEST_ASSERT_FALSE(mm_topic_matches("a/b+", "a/b1"));
    TEST_ASSERT_FALSE(mm_topic_matches("a/b#", "a/b1"));
    TEST_ASSERT_FALSE(mm_topic_matches("", "a"));
    TEST_ASSERT_FALSE(mm_topic_matches(NULL, "a"));
    TEST_ASSERT_FALSE(mm_topic_matches("a", NULL));
}

static void test_wican_contract_filters(void)
{
    /* the filters the product will actually use */
    TEST_ASSERT_TRUE(mm_topic_matches("wican/14c19f44e349/#",
                                      "wican/14c19f44e349/can/tx"));
    TEST_ASSERT_TRUE(mm_topic_matches("wican/+/status",
                                      "wican/14c19f44e349/status"));
    TEST_ASSERT_FALSE(mm_topic_matches("wican/14c19f44e349/#",
                                       "wican/other_device/can/tx"));
}

static void test_url_valid(void)
{
    TEST_ASSERT_TRUE(mm_url_valid("mqtt://10.42.0.1"));
    TEST_ASSERT_TRUE(mm_url_valid("mqtt://broker.local:1883"));
    TEST_ASSERT_TRUE(mm_url_valid("mqtts://io.example.com:8883"));
}

static void test_url_invalid(void)
{
    TEST_ASSERT_FALSE(mm_url_valid(""));
    TEST_ASSERT_FALSE(mm_url_valid(NULL));
    TEST_ASSERT_FALSE(mm_url_valid("http://broker"));
    TEST_ASSERT_FALSE(mm_url_valid("mqtt://"));
    TEST_ASSERT_FALSE(mm_url_valid("mqtt://:1883"));
    TEST_ASSERT_FALSE(mm_url_valid("broker.local:1883"));
}

static void test_item_roundtrip(void)
{
    uint8_t buf[256];
    const char payload[] = "{\"event\":\"bump\"}";
    size_t need = mm_item_size(strlen("wican/x/events"),
                               sizeof(payload) - 1);

    TEST_ASSERT_GREATER_THAN(0, (int)need);
    TEST_ASSERT_TRUE(mm_item_pack(buf, sizeof(buf), "wican/x/events",
                                  payload, sizeof(payload) - 1, 1, true));

    mm_item_hdr_t hdr;
    const char *topic;
    const uint8_t *data;

    TEST_ASSERT_TRUE(mm_item_unpack(buf, need, &hdr, &topic, &data));
    TEST_ASSERT_EQUAL_STRING("wican/x/events", topic);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload) - 1, hdr.data_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, data, hdr.data_len);
    TEST_ASSERT_EQUAL_UINT8(1, hdr.qos);
    TEST_ASSERT_EQUAL_UINT8(1, hdr.retain);
}

static void test_item_bounds(void)
{
    uint8_t buf[64];
    char long_topic[MM_ITEM_TOPIC_MAX + 8];

    memset(long_topic, 'a', sizeof(long_topic) - 1);
    long_topic[sizeof(long_topic) - 1] = '\0';
    TEST_ASSERT_EQUAL_size_t(0, mm_item_size(strlen(long_topic), 4));
    TEST_ASSERT_EQUAL_size_t(0, mm_item_size(0, 4));
    TEST_ASSERT_EQUAL_size_t(0, mm_item_size(4, MM_ITEM_DATA_MAX + 1));
    TEST_ASSERT_FALSE(mm_item_pack(buf, 4, "topic", "data", 4, 0, false));
    TEST_ASSERT_FALSE(mm_item_pack(buf, sizeof(buf), "t", "d", 1, 3,
                                   false));
}

static void test_item_unpack_rejects_corrupt(void)
{
    uint8_t buf[128];
    mm_item_hdr_t hdr;
    const char *topic;
    const uint8_t *data;

    TEST_ASSERT_TRUE(mm_item_pack(buf, sizeof(buf), "a/b", "xy", 2, 0,
                                  false));
    /* truncated */
    TEST_ASSERT_FALSE(mm_item_unpack(buf, 5, &hdr, &topic, &data));
    /* topic loses its NUL */
    buf[sizeof(mm_item_hdr_t) + 3] = 'Z';
    TEST_ASSERT_FALSE(mm_item_unpack(buf, mm_item_size(3, 2), &hdr,
                                     &topic, &data));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exact_match);
    RUN_TEST(test_plus_matches_one_level);
    RUN_TEST(test_hash_matches_rest);
    RUN_TEST(test_dollar_topics_hidden_from_wildcards);
    RUN_TEST(test_malformed_filters_match_nothing);
    RUN_TEST(test_wican_contract_filters);
    RUN_TEST(test_url_valid);
    RUN_TEST(test_url_invalid);
    RUN_TEST(test_item_roundtrip);
    RUN_TEST(test_item_bounds);
    RUN_TEST(test_item_unpack_rejects_corrupt);
    UNITY_END();
}
