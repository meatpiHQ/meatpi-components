/**
 * @file test_dtc_report.c
 * @brief Host suite for the DTC report helpers of autopid_dtc_codec.c
 *        (TASK_j1939_wwh.md phase 3): the merged category arrays that
 *        events, rules and scripts read, the per-ECU items behind them and
 *        the lamp word of every responder. Run from test_main.c's app_main.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

void test_dtc_report_add_merges_and_keeps_sources(void)
{
    static ap_dtc_report_t r;   /* ~5 KB: not a stack frame */

    memset(&r, 0, sizeof(r));

    /* two ECUs of a WWH vehicle: the same P0420 from both, P2463-1F
       pending from the engine alone */
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, "P0420",
                                       0x18DAF100, 0x08, 0x02));
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, "P0420",
                                       0x18DAF13D, 0x0C, 0x04));
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_PENDING, "P2463-1F",
                                       0x18DAF100, 0x04, 0x04));
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_PERMANENT,
                                       "SPN524287-31", 0x18DAF100, 0x08, 0));

    /* the merged view: one code once */
    TEST_ASSERT_EQUAL(1, r.n_stored);
    TEST_ASSERT_EQUAL_STRING("P0420", r.stored[0]);
    TEST_ASSERT_EQUAL(1, r.n_pending);
    TEST_ASSERT_EQUAL_STRING("P2463-1F", r.pending[0]);
    TEST_ASSERT_EQUAL(1, r.n_permanent);
    TEST_ASSERT_EQUAL_STRING("SPN524287-31", r.permanent[0]);

    /* the detail: every ECU's word */
    TEST_ASSERT_EQUAL(4, r.n_items);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF13D, r.items[1].ecu);
    TEST_ASSERT_EQUAL_HEX8(0x0C, r.items[1].status);
    TEST_ASSERT_EQUAL_HEX8(0x04, r.items[1].severity);
    TEST_ASSERT_EQUAL(AP_DTC_KIND_PENDING, r.items[2].kind);

    /* the same word again changes nothing */
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, "P0420",
                                       0x18DAF100, 0x08, 0x02));
    TEST_ASSERT_EQUAL(1, r.n_stored);
    TEST_ASSERT_EQUAL(4, r.n_items);

    /* the same code in another category is another fact */
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_PENDING, "P0420",
                                       0x18DAF100, 0x04, 0x02));
    TEST_ASSERT_EQUAL(2, r.n_pending);
    TEST_ASSERT_EQUAL(5, r.n_items);

    TEST_ASSERT_FALSE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, "", 1, 0, 0));
    TEST_ASSERT_FALSE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, NULL, 1, 0,
                                        0));
    TEST_ASSERT_FALSE(ap_dtc_report_add(NULL, AP_DTC_KIND_STORED, "P0420",
                                        1, 0, 0));

    TEST_ASSERT_EQUAL_STRING("stored", ap_dtc_kind_name(AP_DTC_KIND_STORED));
    TEST_ASSERT_EQUAL_STRING("pending",
                             ap_dtc_kind_name(AP_DTC_KIND_PENDING));
    TEST_ASSERT_EQUAL_STRING("permanent",
                             ap_dtc_kind_name(AP_DTC_KIND_PERMANENT));
}

void test_dtc_report_add_bounds(void)
{
    static ap_dtc_report_t r;
    char code[AP_DTC_CODE_LEN];

    memset(&r, 0, sizeof(r));

    /* a category holds AP_DTC_MAX codes; the next one is refused */
    for (int i = 0; i < AP_DTC_MAX; i++)
    {
        snprintf(code, sizeof(code), "P%04X", 0x100 + i);
        TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, code,
                                           0x7E8, 0, 0));
    }

    TEST_ASSERT_EQUAL(AP_DTC_MAX, r.n_stored);
    TEST_ASSERT_FALSE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, "P0999",
                                        0x7E8, 0, 0));
    TEST_ASSERT_EQUAL(AP_DTC_MAX, r.n_stored);

    /* the item table fills up before the categories do: a code still
       lands in its category, only the per-ECU detail is lost */
    for (int i = 0; i < AP_DTC_MAX; i++)
    {
        snprintf(code, sizeof(code), "C%04X", 0x100 + i);
        TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_PENDING, code,
                                           0x7E9, 0, 0));
    }

    TEST_ASSERT_EQUAL(AP_DTC_MAX, r.n_pending);
    TEST_ASSERT_EQUAL(AP_DTC_ITEMS_MAX, r.n_items);

    /* a code longer than the field is cut, never overruns */
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_PERMANENT,
                                       "SPN524287-31-EXTRA", 0x7E8, 0, 0));
    TEST_ASSERT_EQUAL_size_t(AP_DTC_CODE_LEN - 1, strlen(r.permanent[0]));
}

void test_dtc_report_sources_fold(void)
{
    static ap_dtc_report_t r;

    memset(&r, 0, sizeof(r));

    /* the engine asks for the lamp with one code, the SCR unit does not */
    ap_dtc_report_src(&r, 0x18DAF100, true, 1);
    ap_dtc_report_src(&r, 0x18DAF13D, false, 0);
    TEST_ASSERT_EQUAL(2, r.n_src);
    TEST_ASSERT_EQUAL(2, r.n_ecus);
    TEST_ASSERT_TRUE(r.mil);
    TEST_ASSERT_EQUAL(1, r.mil_count);

    /* a responder seen again keeps its latest word */
    ap_dtc_report_src(&r, 0x18DAF100, false, 0);
    TEST_ASSERT_EQUAL(2, r.n_src);
    TEST_ASSERT_FALSE(r.mil);
    TEST_ASSERT_EQUAL(0, r.mil_count);

    /* the sum saturates; a ninth responder is not kept */
    for (uint32_t i = 0; i < 6; i++)
    {
        ap_dtc_report_src(&r, 0x7E8 + i, true, 100);
    }

    TEST_ASSERT_EQUAL(AP_DTC_SRC_MAX, r.n_src);
    TEST_ASSERT_EQUAL(255, r.mil_count);
    ap_dtc_report_src(&r, 0x7EF, true, 1);
    TEST_ASSERT_EQUAL(AP_DTC_SRC_MAX, r.n_src);
    ap_dtc_report_src(NULL, 0x7E8, true, 1);
}

void test_dtc_report_j1939_codes_and_lamps(void)
{
    static ap_dtc_report_t r;   /* ~5 KB: not a stack frame */

    memset(&r, 0, sizeof(r));
    TEST_ASSERT_FALSE(r.j1939);

    /* the engine (SA 0): amber warning + MIL, two active codes; the brakes
       (SA 11): no lamp, one code; the engine's code again = one item */
    ap_dtc_report_src_j1939(&r, 0, AP_DTC_LAMP_MIL | AP_DTC_LAMP_AWL, 2);
    TEST_ASSERT_TRUE(ap_dtc_report_add_j1939(&r, "SPN110-0", 0, 5, false));
    TEST_ASSERT_TRUE(ap_dtc_report_add_j1939(&r, "SPN3226-4", 0, 1, false));
    ap_dtc_report_src_j1939(&r, 11, 0, 1);
    TEST_ASSERT_TRUE(ap_dtc_report_add_j1939(&r, "SPN520192-31", 11, 127,
                                             false));
    TEST_ASSERT_TRUE(ap_dtc_report_add_j1939(&r, "SPN110-0", 0, 6, false));

    TEST_ASSERT_TRUE(r.j1939);
    TEST_ASSERT_TRUE(r.mil);                       /* the engine's MIL   */
    TEST_ASSERT_EQUAL(AP_DTC_LAMP_MIL | AP_DTC_LAMP_AWL, r.lamps);
    TEST_ASSERT_EQUAL(2, r.n_src);
    TEST_ASSERT_EQUAL(3, r.mil_count);             /* 2 + 1              */
    TEST_ASSERT_TRUE(r.src[0].j1939);
    TEST_ASSERT_EQUAL(0, r.src[0].ecu);
    TEST_ASSERT_TRUE(r.src[0].mil);
    TEST_ASSERT_TRUE(r.src[1].j1939);
    TEST_ASSERT_EQUAL(11, r.src[1].ecu);
    TEST_ASSERT_FALSE(r.src[1].mil);
    TEST_ASSERT_EQUAL(0, r.src[1].lamps);

    /* stored: three distinct codes; items: three, each a DM1 item */
    TEST_ASSERT_EQUAL(3, r.n_stored);
    TEST_ASSERT_EQUAL_STRING("SPN110-0", r.stored[0]);
    TEST_ASSERT_EQUAL_STRING("SPN3226-4", r.stored[1]);
    TEST_ASSERT_EQUAL_STRING("SPN520192-31", r.stored[2]);
    TEST_ASSERT_EQUAL(3, r.n_items);
    TEST_ASSERT_TRUE(r.items[0].j1939);
    TEST_ASSERT_EQUAL(0, r.items[0].ecu);
    TEST_ASSERT_EQUAL(6, r.items[0].oc);           /* the repeat updated it */
    TEST_ASSERT_EQUAL(AP_DTC_KIND_STORED, r.items[0].kind);
    TEST_ASSERT_TRUE(r.items[2].j1939);
    TEST_ASSERT_EQUAL(11, r.items[2].ecu);
    TEST_ASSERT_EQUAL(127, r.items[2].oc);

    /* an OBD responder beside them (an EU truck): its word folds as before,
       its item is not a DM1 item */
    ap_dtc_report_src(&r, 0x18DAF100, false, 1);
    TEST_ASSERT_TRUE(ap_dtc_report_add(&r, AP_DTC_KIND_STORED, "P0420-08",
                                       0x18DAF100, 0x08, 0));
    TEST_ASSERT_EQUAL(3, r.n_src);
    TEST_ASSERT_FALSE(r.src[2].j1939);
    TEST_ASSERT_FALSE(r.items[3].j1939);
    TEST_ASSERT_TRUE(r.mil);
    TEST_ASSERT_EQUAL(4, r.mil_count);

    /* the lamps follow a controller that turns its lamp off */
    ap_dtc_report_src_j1939(&r, 0, AP_DTC_LAMP_AWL, 2);
    TEST_ASSERT_EQUAL(AP_DTC_LAMP_AWL, r.lamps);
    TEST_ASSERT_FALSE(r.mil);

    /* a previously active code (DM2, active mode): pending, same item
       shape; the same code active AND previously active = two items */
    TEST_ASSERT_TRUE(ap_dtc_report_add_j1939(&r, "SPN100-1", 0, 2, true));
    TEST_ASSERT_TRUE(ap_dtc_report_add_j1939(&r, "SPN110-0", 0, 9, true));
    TEST_ASSERT_EQUAL(2, r.n_pending);
    TEST_ASSERT_EQUAL_STRING("SPN100-1", r.pending[0]);
    TEST_ASSERT_EQUAL_STRING("SPN110-0", r.pending[1]);
    TEST_ASSERT_EQUAL(4, r.n_stored);              /* unchanged: 3 + P0420 */
    TEST_ASSERT_EQUAL(6, r.n_items);
    TEST_ASSERT_TRUE(r.items[4].j1939);
    TEST_ASSERT_EQUAL(AP_DTC_KIND_PENDING, r.items[4].kind);
    TEST_ASSERT_EQUAL(2, r.items[4].oc);
    TEST_ASSERT_EQUAL(AP_DTC_KIND_PENDING, r.items[5].kind);
    TEST_ASSERT_EQUAL(9, r.items[5].oc);
    TEST_ASSERT_EQUAL(6, r.items[0].oc);           /* the active one kept */

    TEST_ASSERT_FALSE(ap_dtc_report_add_j1939(&r, "", 0, 0, false));
    TEST_ASSERT_FALSE(ap_dtc_report_add_j1939(NULL, "SPN1-1", 0, 0, false));
    ap_dtc_report_src_j1939(NULL, 0, 0, 0);
}

void run_dtc_report_tests(void)
{
    RUN_TEST(test_dtc_report_add_merges_and_keeps_sources);
    RUN_TEST(test_dtc_report_add_bounds);
    RUN_TEST(test_dtc_report_sources_fold);
    RUN_TEST(test_dtc_report_j1939_codes_and_lamps);
}
