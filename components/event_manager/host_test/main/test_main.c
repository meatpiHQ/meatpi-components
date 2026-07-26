/**
 * @file test_main.c
 * @brief Host suite for event_manager's pure modules: rule
 *        parse/validate, match/when evaluation (every op incl.
 *        `changed`), cooldown arithmetic (fake µs clock), template
 *        substitution.
 */
#include <string.h>

#include "unity.h"

#include "event_manager_private.h"

static em_rule_t s_rules[EM_RULES_MAX];
static int s_count;
static char s_err[96];

static esp_err_t parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    esp_err_t rc = em_rules_parse(root, s_rules, EM_RULES_MAX, &s_count,
                                  s_err, sizeof(s_err));

    cJSON_Delete(root);
    return rc;
}

static em_event_t ev_param(const char *param, double value)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "param");
    ev.ts_us = 123456789;
    ev.kv[0] = (em_kv_t){ .key = "param", .type = EM_VAL_STR };
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "%s", param);
    ev.kv[1] = (em_kv_t){ .key = "value", .type = EM_VAL_F64 };
    ev.kv[1].v.f64 = value;
    ev.n = 2;
    return ev;
}

/* ---- parsing ------------------------------------------------------------------- */

void test_rules_parse_happy(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"high_rpm\",\"on\":\"autopid.param\","
        "\"match\":{\"param\":\"rpm\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":3000}],"
        "\"do\":\"log.note\",\"with\":{\"message\":\"rpm ${value}\"},"
        "\"cooldown_ms\":60000}]"));
    TEST_ASSERT_EQUAL(1, s_count);
    TEST_ASSERT_TRUE(s_rules[0].enabled);
    TEST_ASSERT_EQUAL(1, s_rules[0].n_match);
    TEST_ASSERT_EQUAL(1, s_rules[0].n_when);
    TEST_ASSERT_EQUAL(EM_OP_GT, s_rules[0].when[0].op);
    TEST_ASSERT_EQUAL(60000, s_rules[0].cooldown_ms);
    TEST_ASSERT_TRUE(strstr(s_rules[0].with_json, "${value}") != NULL);
}

void test_rules_parse_rejects(void)
{
    /* missing dot in selector */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"autopid\",\"do\":\"log.note\"}]"));
    /* unknown op */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"key\":\"v\",\"op\":\"~=\",\"val\":1}]}]"));
    /* ordered op with a string operand */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"key\":\"v\",\"op\":\">\",\"val\":\"hi\"}]}]"));
    /* duplicate names */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\"},"
        "{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\"}]"));
    /* empty array + null are fine */
    TEST_ASSERT_EQUAL(ESP_OK, parse("[]"));
    TEST_ASSERT_EQUAL(0, s_count);
}

void test_rules_parse_script_sugar(void)
{
    /* {"script":"name"} rewrites to script.run + {"name":...} */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"diag\",\"on\":\"uds.response\","
        "\"script\":\"vin_check\"}]"));
    TEST_ASSERT_EQUAL(1, s_count);
    TEST_ASSERT_EQUAL_STRING("script.run", s_rules[0].action);
    TEST_ASSERT_EQUAL_STRING("{\"name\":\"vin_check\"}",
                             s_rules[0].with_json);

    /* script + do are exclusive */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"script\":\"s\","
        "\"do\":\"log.note\"}]"));
    /* script takes no with */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"script\":\"s\","
        "\"with\":{\"k\":1}}]"));
    /* quote/backslash injection rejected */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\","
        "\"script\":\"a\\\"b\"}]"));
    /* neither script nor do */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\"}]"));
}

/* ---- match + when --------------------------------------------------------------- */

void test_match_and_ops(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"autopid.param\","
        "\"match\":{\"param\":\"rpm\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":3000}],"
        "\"do\":\"log.note\"}]"));

    em_rule_state_t st = { 0 };
    em_event_t hi = ev_param("rpm", 4000);
    em_event_t lo = ev_param("rpm", 1000);
    em_event_t other = ev_param("speed", 9000);

    TEST_ASSERT_TRUE(em_rule_match(&s_rules[0], &hi));
    TEST_ASSERT_FALSE(em_rule_match(&s_rules[0], &other));
    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &hi, &st));
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &lo, &st));

    /* every comparison op over the same event */
    struct { const char *op; double val; bool expect; } cases[] =
    {
        { "==", 4000, true },  { "==", 1, false },
        { "!=", 1, true },     { "!=", 4000, false },
        { ">=", 4000, true },  { "<", 4000, false },
        { "<=", 4000, true },  { ">", 4000, false },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char json[220];

        snprintf(json, sizeof(json),
                 "[{\"name\":\"r\",\"on\":\"autopid.param\","
                 "\"when\":[{\"key\":\"value\",\"op\":\"%s\","
                 "\"val\":%g}],\"do\":\"log.note\"}]",
                 cases[i].op, cases[i].val);
        TEST_ASSERT_EQUAL(ESP_OK, parse(json));

        em_rule_state_t s2 = { 0 };

        TEST_ASSERT_EQUAL_MESSAGE(cases[i].expect,
                                  em_rule_when(&s_rules[0], &hi, &s2),
                                  cases[i].op);
    }
}

void test_contains_and_string_eq(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"mqtt.rx\","
        "\"when\":[{\"key\":\"payload\",\"op\":\"contains\","
        "\"val\":\"launch\"}],\"do\":\"log.note\"}]"));

    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "mqtt");
    snprintf(ev.name, sizeof(ev.name), "rx");
    ev.kv[0] = (em_kv_t){ .key = "payload", .type = EM_VAL_STR };
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "please launch now");
    ev.n = 1;

    em_rule_state_t st = { 0 };

    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &ev, &st));
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "nothing here");
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &ev, &st));
}

void test_changed_semantics(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"autopid.param\","
        "\"when\":[{\"key\":\"value\",\"op\":\"changed\"}],"
        "\"do\":\"log.note\"}]"));

    em_rule_state_t st = { 0 };
    em_event_t a = ev_param("rpm", 100);
    em_event_t b = ev_param("rpm", 200);

    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &a, &st));  /* first    */
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &a, &st)); /* same     */
    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &b, &st));  /* changed  */
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &b, &st));
    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &a, &st));  /* back     */
}

void test_cooldown_arithmetic(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"cooldown_ms\":1000}]"));

    em_rule_state_t st = { 0 };

    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &st, 5000000));
    TEST_ASSERT_FALSE(em_rule_cooldown_ok(&s_rules[0], &st, 5500000));
    TEST_ASSERT_FALSE(em_rule_cooldown_ok(&s_rules[0], &st, 5999999));
    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &st, 6000001));

    /* no cooldown = always allowed */
    s_rules[0].cooldown_ms = 0;

    em_rule_state_t s2 = { 0 };

    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &s2, 1));
    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &s2, 2));
}

/* ---- templates -------------------------------------------------------------------- */

static esp_err_t fake_resolver(const char *name, char *out, size_t len)
{
    if (strcmp(name, "battery.voltage") == 0)
    {
        snprintf(out, len, "12.43");
        return ESP_OK;
    }

    if (strcmp(name, "autopid.data") == 0)
    {
        snprintf(out, len, "{\"rpm\":5324}");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

void test_template_render(void)
{
    em_event_t ev = ev_param("rpm", 4000.5);
    char out[256];

    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "{\"p\":\"${param}\",\"v\":${value},\"at\":${ts}}", &ev,
        fake_resolver, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"p\":\"rpm\",\"v\":4000.5,\"at\":123456789}", out);

    /* pull values + missing key -> null */
    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "batt=${battery.voltage} data=${autopid.data} x=${nope}", &ev,
        fake_resolver, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "batt=12.43 data={\"rpm\":5324} x=null", out);

    /* no templates = verbatim; ${ without } = literal */
    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "plain ${unclosed", &ev, fake_resolver, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("plain ${unclosed", out);

    /* overflow detected */
    char tiny[8];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, em_template_render(
        "0123456789", &ev, NULL, tiny, sizeof(tiny)));
}

void test_template_types(void)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "s");
    snprintf(ev.name, sizeof(ev.name), "n");
    ev.kv[0] = (em_kv_t){ .key = "b", .type = EM_VAL_BOOL };
    ev.kv[0].v.b = true;
    ev.kv[1] = (em_kv_t){ .key = "i", .type = EM_VAL_I64 };
    ev.kv[1].v.i64 = -42;
    ev.n = 2;

    char out[64];

    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "${b}/${i}/${source}.${name}", &ev, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("true/-42/s.n", out);
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_rules_parse_happy);
    RUN_TEST(test_rules_parse_rejects);
    RUN_TEST(test_rules_parse_script_sugar);
    RUN_TEST(test_match_and_ops);
    RUN_TEST(test_contains_and_string_eq);
    RUN_TEST(test_changed_semantics);
    RUN_TEST(test_cooldown_arithmetic);
    RUN_TEST(test_template_render);
    RUN_TEST(test_template_types);

    UNITY_END();
}
