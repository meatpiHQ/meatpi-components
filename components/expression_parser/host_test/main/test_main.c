/**
 * @file test_main.c
 * @brief Host suite for expression_parser: the legacy vector table
 *        (grammar compatibility), docs-page vectors, and the adversarial
 *        cases the length parameter exists for (meatpi 2026-07-06).
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "expression_parser.h"

/* The legacy test payload (autopid_legacy/expression_parser.c) */
static const uint8_t DATA[] =
{
    0xFF, 0x00, 0xF0, 0x0F, 0xAA, 0x55, 0x33, 0x77, 0x88,
    0xCC, 0x99, 0xEE, 0x44, 0x85, 0x06, 0x00, 0xEF
};
#define DATA_LEN sizeof(DATA)
#define V_TEST   3.3

static void expect_value(const char *expr, double expected)
{
    double result = 0;
    esp_err_t err = expression_parser_eval(expr, DATA, DATA_LEN, V_TEST,
                                           &result);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err, expr);

    /* hand-rolled double compare: Unity's double asserts are config-gated
       on IDF; relative term keeps 40-bit range values honest */
    double tol = 1e-6 + 1e-9 * fabs(expected);

    if (fabs(expected - result) > tol)
    {
        printf("MISMATCH %s: expected %.9f got %.9f\n", expr, expected,
               result);
        TEST_FAIL_MESSAGE(expr);
    }
}

static void expect_error(const char *expr, esp_err_t expected_err)
{
    double result = 0;
    esp_err_t err = expression_parser_eval(expr, DATA, DATA_LEN, V_TEST,
                                           &result);

    TEST_ASSERT_EQUAL_MESSAGE(expected_err, err, expr);
}

/* ---- the legacy vector table (grammar compatibility) ----------------------- */

void test_legacy_arithmetic(void)
{
    expect_value("3 + 5", 8.0);
    expect_value("10 - 2 * 3", 4.0);
    expect_value("(3 + 5) * 2", 16.0);
    expect_value("10 / 2", 5.0);
    expect_value("V + 2", 5.3);
    expect_value("(2 + 3) * (7 - 2)", 25.0);
    expect_value("((5 + 5) / 2) * 3 - 1", 14.0);
}

void test_legacy_bitwise_and_shifts(void)
{
    expect_value("3 & 1", 1.0);
    expect_value("3 | 4", 7.0);
    expect_value("5 ^ 2", 7.0);
    expect_value("2 << 1", 4.0);
    expect_value("8 >> 2", 2.0);
    expect_value("B2 << 4", 3840.0);
    expect_value("B3 >> 1", 7.0);
}

void test_legacy_byte_access(void)
{
    expect_value("B0", 255.0);
    expect_value("B5", 85.0);
    expect_value("B6 + B7", (double)(0x33 + 0x77));
    expect_value("B8 - B9", (double)(0x88 - 0xCC));
    expect_value("B5 * B12", 85.0 * 68.0);
    expect_value("B0 / B2", 255.0 / 240.0);
    expect_value("B16", (double)0xEF); /* last valid index */
}

void test_legacy_bit_extraction(void)
{
    expect_value("B4:1", 1.0);  /* 0xAA bit1 */
    expect_value("B4:0", 0.0);  /* 0xAA bit0 */
    expect_value("B4:7", 1.0);  /* 0xAA bit7 */
}

void test_legacy_ranges_unsigned(void)
{
    expect_value("[B1:B3]", (double)0x00F00F);
    expect_value("[B5:B6]", (double)0x5533);
    expect_value("[B0:B4]", (double)0xFF00F00FAAULL);
    expect_value("[B3:B6] >> 8", (double)(0x0FAA5533u >> 8));
    expect_value("[B0:B0]", 255.0); /* single-byte range */
}

void test_legacy_ranges_signed(void)
{
    /* container by span — the exact legacy semantics */
    expect_value("S0", -1.0);
    expect_value("S2", -16.0);
    expect_value("S16", (double)(int8_t)0xEF);
    expect_value("S4 + S5", (double)((int8_t)0xAA + (int8_t)0x55));
    expect_value("S6 - S7", (double)((int8_t)0x33 - (int8_t)0x77));
    expect_value("S11 - S14", (double)((int8_t)0xEE - (int8_t)0x06));
    /* 3-byte span -> int32 container (no sign bit in range, as legacy) */
    expect_value("[S1:S3]", (double)(int32_t)0x00F00F);
    /* 4-byte span -> int32 with the real sign bit */
    expect_value("[S12:S15]", (double)(int32_t)0x44850600);
    /* 2-byte span -> int16 */
    expect_value("[S8:S9]", (double)(int16_t)0x88CC);
    /* 1-byte span -> int8 */
    expect_value("[S0:S0]", -1.0);
}

void test_legacy_complex_combinations(void)
{
    expect_value("(B1 + B2) * (B3 - B4) / 2",
                 ((0x00 + 0xF0) * (0x0F - 0xAA)) / 2.0);
    expect_value("(V * B0) / B3", (V_TEST * 255) / 0x0F);
    expect_value("(3 + V) * (B0 & 15) / 2", (3 + V_TEST) * 15 / 2);
    expect_value("B10 + B11 * B12", 0x99 + (0xEE * 0x44));
    expect_value("((B0 & B1) | B2) ^ B3", ((0xFF & 0x00) | 0xF0) ^ 0x0F);
    expect_value("(B4 + B5) << (B6 & 3)", (0xAA + 0x55) << (0x33 & 3));
}

void test_legacy_error_cases(void)
{
    expect_error("10 / 0", ESP_FAIL);
    expect_error("B5 / 0", ESP_FAIL);
    expect_error("3*-2", ESP_ERR_INVALID_ARG); /* legacy errored here too */
}

/* ---- docs-page shapes (real formulas users write) --------------------------- */

void test_docs_formulas(void)
{
    /* engine RPM: [B0:B1]/4 — with this payload (0xFF00) */
    expect_value("[B0:B1]/4", (double)0xFF00 / 4.0);
    /* coolant temp: B0-40 */
    expect_value("B0-40", 255.0 - 40.0);
    /* throttle: B0*100/255 */
    expect_value("B0*100/255", 100.0);
    /* battery from the V variable */
    expect_value("V*1000", 3300.0);
}

/* ---- unary minus (defined behavior; legacy accepted these by accident) ------ */

void test_unary_minus(void)
{
    expect_value("-5", -5.0);
    expect_value("-5*2", -10.0);      /* == legacy accidental result */
    expect_value("(-5+3)", -2.0);
    expect_value("-B3", -15.0);
    expect_value("3*(0-2)", -6.0);    /* the documented spelling      */
}

/* ---- the length parameter (the reason this component exists) ---------------- */

void test_out_of_range_refs_rejected(void)
{
    double result = 0;

    /* B17 is one past the 17-byte payload */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("B17", DATA, DATA_LEN, 0,
                                             &result));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("S17", DATA, DATA_LEN, 0,
                                             &result));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("[B15:B17]", DATA, DATA_LEN, 0,
                                             &result));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("B4:1", DATA, 4, 0, &result));
    /* same expressions fit a longer payload */
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_eval("B3", DATA, 4, 0, &result));
    /* short payload: index == len is out, len-1 is in */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("B4", DATA, 4, 0, &result));
    /* empty payload: any ref is out; pure math still works */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      expression_parser_eval("B0", NULL, 0, 0, &result));
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_eval("1+2", NULL, 0, 0, &result));
}

void test_malformed_expressions_rejected(void)
{
    expect_error("", ESP_ERR_INVALID_ARG);
    expect_error("3 +", ESP_ERR_INVALID_ARG);
    expect_error("(3+2", ESP_ERR_INVALID_ARG);
    expect_error("3+2)", ESP_ERR_INVALID_ARG);
    expect_error("B", ESP_ERR_INVALID_ARG);        /* no index          */
    expect_error("B4:8", ESP_ERR_INVALID_ARG);     /* bit > 7           */
    expect_error("B4:x", ESP_ERR_INVALID_ARG);
    expect_error("[B3:B0]", ESP_ERR_INVALID_ARG);  /* inverted range    */
    expect_error("[B0:B8]", ESP_ERR_INVALID_ARG);  /* span 9 > 8 bytes  */
    expect_error("[B0-B3]", ESP_ERR_INVALID_ARG);  /* bad range syntax  */
    expect_error("[X0:X1]", ESP_ERR_INVALID_ARG);
    expect_error("2 ** 3", ESP_ERR_INVALID_ARG);
    expect_error("hello", ESP_ERR_INVALID_ARG);
    expect_error("3 $ 4", ESP_ERR_INVALID_ARG);
}

void test_null_args(void)
{
    double result;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      expression_parser_eval(NULL, DATA, DATA_LEN, 0,
                                             &result));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      expression_parser_eval("1", DATA, DATA_LEN, 0, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      expression_parser_eval("1", NULL, 4, 0, &result));
}

/* ---- check(): save-time validation ------------------------------------------ */

void test_check_reports_max_byte(void)
{
    size_t max_byte = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_check("((B4+B5) << (B6 & 3)) + [B10:B12]",
                                              &max_byte, NULL, 0));
    TEST_ASSERT_EQUAL(12, max_byte);

    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_check("B0-40", &max_byte, NULL, 0));
    TEST_ASSERT_EQUAL(0, max_byte);

    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_check("B3:0 + S9", &max_byte, NULL,
                                              0));
    TEST_ASSERT_EQUAL(9, max_byte);

    /* no byte references at all -> SIZE_MAX sentinel */
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_check("V*2 + 1", &max_byte, NULL,
                                              0));
    TEST_ASSERT_EQUAL(SIZE_MAX, max_byte);
}

void test_check_dry_run_semantics(void)
{
    char err[64] = "";

    /* division by a byte value must NOT fail in the dry run (values are
       fake zeros) — that is precisely why eval and check share a core */
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_check("100/B3", NULL, err,
                                              sizeof(err)));

    /* ...but a LITERAL /0 passes the check too (runtime concern only) */
    TEST_ASSERT_EQUAL(ESP_OK,
                      expression_parser_check("1/0", NULL, err,
                                              sizeof(err)));

    /* syntax errors report a human-readable reason */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      expression_parser_check("[B3:B0]", NULL, err,
                                              sizeof(err)));
    TEST_ASSERT_TRUE(strlen(err) > 0);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      expression_parser_check(NULL, NULL, err, sizeof(err)));
}

/* ---- precedence pinning (regression net for the shared core) ---------------- */

void test_precedence_table(void)
{
    expect_value("2+3*4", 14.0);
    expect_value("8 >> 2 + 1", 1.0);      /* shifts bind looser than +  */
    expect_value("3 & 1 + 1", 2.0);       /* & looser than +: 3&(1+1)   */
    expect_value("1 | 2 ^ 3", 0.0);       /* same tier, left-to-right   */
    expect_value("2 << 1 & 3", 0.0);      /* & looser: (2<<1)&3 = 0     */
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_legacy_arithmetic);
    RUN_TEST(test_legacy_bitwise_and_shifts);
    RUN_TEST(test_legacy_byte_access);
    RUN_TEST(test_legacy_bit_extraction);
    RUN_TEST(test_legacy_ranges_unsigned);
    RUN_TEST(test_legacy_ranges_signed);
    RUN_TEST(test_legacy_complex_combinations);
    RUN_TEST(test_legacy_error_cases);
    RUN_TEST(test_docs_formulas);
    RUN_TEST(test_unary_minus);
    RUN_TEST(test_out_of_range_refs_rejected);
    RUN_TEST(test_malformed_expressions_rejected);
    RUN_TEST(test_null_args);
    RUN_TEST(test_check_reports_max_byte);
    RUN_TEST(test_check_dry_run_semantics);
    RUN_TEST(test_precedence_table);

    UNITY_END();
}
