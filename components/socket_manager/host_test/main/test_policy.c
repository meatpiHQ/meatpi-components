/**
 * @file test_policy.c
 * @brief Host tests for socket_manager's pure policy layer: backoff
 *        progression, accept decisions, config parse + cross-item rules.
 */
#include <string.h>

#include "unity.h"

#include "socket_manager.h"
#include "socket_manager_private.h"

/* ---- backoff ------------------------------------------------------------------ */

static void test_backoff_progression_and_cap(void)
{
    uint32_t ms = smp_backoff_next_ms(0);

    TEST_ASSERT_EQUAL(1000, ms);
    ms = smp_backoff_next_ms(ms);
    TEST_ASSERT_EQUAL(2000, ms);
    ms = smp_backoff_next_ms(ms);
    TEST_ASSERT_EQUAL(4000, ms);
    ms = smp_backoff_next_ms(ms);
    TEST_ASSERT_EQUAL(8000, ms);
    ms = smp_backoff_next_ms(ms);
    TEST_ASSERT_EQUAL(8000, ms); /* capped */
}

/* ---- accept decision ------------------------------------------------------------ */

static void test_accept_until_max_then_reject(void)
{
    TEST_ASSERT_EQUAL(SMP_ACCEPT, smp_accept_decision(0, 2));
    TEST_ASSERT_EQUAL(SMP_ACCEPT, smp_accept_decision(1, 2));
    TEST_ASSERT_EQUAL(SMP_REJECT, smp_accept_decision(2, 2));
    TEST_ASSERT_EQUAL(SMP_REJECT, smp_accept_decision(3, 2));
}

/* ---- parse -------------------------------------------------------------------- */

static void test_parse_fills_code_defaults(void)
{
    cJSON *item = cJSON_Parse(
        "{\"name\":\"obd0\",\"proto\":\"tcp\",\"port\":35000}");
    smp_server_cfg_t cfg;

    TEST_ASSERT_EQUAL(ESP_OK, smp_parse_server(item, &cfg));
    TEST_ASSERT_EQUAL_STRING("obd0", cfg.name);
    TEST_ASSERT_FALSE(cfg.is_udp);
    TEST_ASSERT_EQUAL(35000, cfg.port);
    TEST_ASSERT_EQUAL(2, cfg.max_clients);   /* code default */
    TEST_ASSERT_EQUAL(30, cfg.keepalive_s);  /* code default */
    TEST_ASSERT_FALSE(cfg.enabled);          /* code default */
    cJSON_Delete(item);
}

static void test_parse_rejects_bad_name(void)
{
    cJSON *item = cJSON_Parse(
        "{\"name\":\"OBD-0!\",\"proto\":\"tcp\",\"port\":1}");
    smp_server_cfg_t cfg;

    TEST_ASSERT_NOT_EQUAL(ESP_OK, smp_parse_server(item, &cfg));
    cJSON_Delete(item);
}

/* ---- cross-item validation ------------------------------------------------------- */

static esp_err_t validate(const char *json, char *err, size_t err_len)
{
    cJSON *arr = cJSON_Parse(json);
    esp_err_t r = smp_validate_servers(arr, err, err_len);

    cJSON_Delete(arr);
    return r;
}

static void test_valid_server_set_accepted(void)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"obd0\",\"proto\":\"tcp\",\"port\":35000,"
          "\"enabled\":true},"
         "{\"name\":\"udp0\",\"proto\":\"udp\",\"port\":17,"
          "\"enabled\":true}]", err, sizeof(err)));
}

static void test_duplicate_name_rejected(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"proto\":\"tcp\",\"port\":1},"
         "{\"name\":\"a\",\"proto\":\"udp\",\"port\":2}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "duplicate"));
}

static void test_enabled_port_clash_rejected(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"proto\":\"tcp\",\"port\":23,\"enabled\":true},"
         "{\"name\":\"b\",\"proto\":\"tcp\",\"port\":23,\"enabled\":true}]",
        err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "port"));
}

static void test_disabled_port_park_allowed(void)
{
    char err[96] = "";

    /* a disabled server may park the same port an enabled one uses */
    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"proto\":\"tcp\",\"port\":23,\"enabled\":true},"
         "{\"name\":\"b\",\"proto\":\"tcp\",\"port\":23,\"enabled\":false}]",
        err, sizeof(err)));
}

static void test_max_clients_bounds_enforced(void)
{
    char err[96] = "";

    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"proto\":\"tcp\",\"port\":1,"
          "\"max_clients\":9}]", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "max_clients"));
}

static void test_httpd_port_rejected(void)
{
    char err[96] = "";

    /* :80 = httpd; lwip SO_REUSEADDR would let both LISTEN and split
       the SYNs nondeterministically (live find, 2026-07-26) */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"proto\":\"tcp\",\"port\":80,"
          "\"enabled\":true}]", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "web server"));

    /* disabled park + udp:80 both stay legal */
    TEST_ASSERT_EQUAL(ESP_OK, validate(
        "[{\"name\":\"a\",\"proto\":\"tcp\",\"port\":80,"
          "\"enabled\":false},"
         "{\"name\":\"b\",\"proto\":\"udp\",\"port\":80,"
          "\"enabled\":true}]", err, sizeof(err)));
}

void run_policy_tests(void)
{
    RUN_TEST(test_backoff_progression_and_cap);
    RUN_TEST(test_accept_until_max_then_reject);
    RUN_TEST(test_parse_fills_code_defaults);
    RUN_TEST(test_parse_rejects_bad_name);
    RUN_TEST(test_valid_server_set_accepted);
    RUN_TEST(test_duplicate_name_rejected);
    RUN_TEST(test_enabled_port_clash_rejected);
    RUN_TEST(test_disabled_port_park_allowed);
    RUN_TEST(test_max_clients_bounds_enforced);
    RUN_TEST(test_httpd_port_rejected);
}
