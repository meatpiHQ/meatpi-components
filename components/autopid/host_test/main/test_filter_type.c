/**
 * @file test_filter_type.c
 * @brief A CAN filter follows the switch of its table (2026-11-14):
 *        `"profile": true` marks a filter of the vehicle's set, gated by
 *        the vehicle-specific switch; every other filter is gated by the
 *        custom one. The line a test of a filter shows for a captured
 *        frame (ap_flt_frame_line), read back by the monitor-line parser.
 *        And the protocol a window opens on (ap_flt_window_proto): the
 *        one of the id's width, as the legacy firmware chose it.
 */
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

/* ~1 MB (the 2048-parameter pool): static, never on the task stack */
static ap_config_t s_cfg;
static ap_sched_t s_st;

#define PRM(n) "\"parameters\":[{\"name\":\"" n "\",\"expression\":\"B0\"}]"

static void load(const char *json)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                              ap_config_parse(json, &s_cfg, err, sizeof(err)),
                              err);
    memset(&s_st, 0, sizeof(s_st));
    ap_sched_reset(&s_st, &s_cfg, 0);
    s_st.type_enabled[AP_PID_STD] = true;
    s_st.type_enabled[AP_PID_CUSTOM] = true;
    s_st.type_enabled[AP_PID_SPECIFIC] = true;
}

void test_filter_type_from_the_profile_tag(void)
{
    /* the tag the web UI writes; a "name" on a filter is the page's label
       and is not the parser's business */
    load("{\"filters\":["
         "{\"frame_id\":644,\"profile\":true,\"name\":\"SPEED\"," PRM("a") "},"
         "{\"frame_id\":645,\"profile\":false," PRM("b") "},"
         "{\"frame_id\":646," PRM("c") "}]}");

    TEST_ASSERT_EQUAL(3, s_cfg.n_filters);
    TEST_ASSERT_EQUAL(AP_PID_SPECIFIC, s_cfg.filters[0].type);
    TEST_ASSERT_EQUAL(AP_PID_CUSTOM, s_cfg.filters[1].type);
    TEST_ASSERT_EQUAL(AP_PID_CUSTOM, s_cfg.filters[2].type);
}

void test_filter_type_follows_its_switch(void)
{
    /* entry 0 = the custom PID, 1 = the vehicle's filter, 2 = a custom one */
    load("{\"pids\":[{\"name\":\"p\",\"type\":\"custom\",\"cmd\":\"22F190\","
         PRM("p") "}],"
         "\"filters\":["
         "{\"frame_id\":644,\"profile\":true," PRM("a") "},"
         "{\"frame_id\":645," PRM("b") "}]}");

    TEST_ASSERT_EQUAL(1, s_cfg.n_pids);

    for (int i = 0; i < 3; i++)
    {
        TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, i));
    }

    /* "Poll vehicle-specific PIDs" off: its filter rests, the others go on */
    s_st.type_enabled[AP_PID_SPECIFIC] = false;
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, 0));
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&s_st, &s_cfg, 1));
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, 2));

    /* "Poll custom PIDs" off: the custom PID and the custom filter rest */
    s_st.type_enabled[AP_PID_SPECIFIC] = true;
    s_st.type_enabled[AP_PID_CUSTOM] = false;
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&s_st, &s_cfg, 0));
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, 1));
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&s_st, &s_cfg, 2));

    /* the standard switch is about no filter */
    s_st.type_enabled[AP_PID_CUSTOM] = true;
    s_st.type_enabled[AP_PID_STD] = false;
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, 1));
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, 2));

    /* a held init (it carries a word the chip stores) keeps the rows that
       are sent behind it waiting; a window sends no init and goes on */
    s_st.type_enabled[AP_PID_STD] = true;
    s_st.type_held[AP_PID_CUSTOM] = true;
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&s_st, &s_cfg, 0));
    TEST_ASSERT_TRUE(ap_sched_entry_enabled(&s_st, &s_cfg, 2));

    /* its own switch still wins */
    s_st.type_held[AP_PID_CUSTOM] = false;
    s_cfg.filters[1].enabled = false;
    TEST_ASSERT_FALSE(ap_sched_entry_enabled(&s_st, &s_cfg, 2));
}

void test_filter_type_gated_filter_is_never_picked(void)
{
    load("{\"filters\":["
         "{\"frame_id\":644,\"profile\":true," PRM("a") "},"
         "{\"frame_id\":645," PRM("b") "}]}");

    int64_t due = 0;

    s_st.type_enabled[AP_PID_SPECIFIC] = false;

    /* the custom filter is the only thing left to run, round after round */
    for (int round = 0; round < 6; round++)
    {
        int i = ap_sched_next(&s_st, &s_cfg, &due);

        TEST_ASSERT_EQUAL(1, i);
        ap_sched_ran(&s_st, &s_cfg, i, due + 1000, true);
    }

    /* both switches off: nothing to run */
    s_st.type_enabled[AP_PID_CUSTOM] = false;
    TEST_ASSERT_EQUAL(-1, ap_sched_next(&s_st, &s_cfg, &due));
}

void test_flt_frame_line(void)
{
    const uint8_t data[8] = { 0x02, 0x1E, 0x02, 0x1F, 0x0B, 0x7C, 0x4E, 0x20 };
    char line[64];
    uint8_t back[16];
    size_t n = 0;

    /* an 11-bit id: three digits, then the data bytes */
    TEST_ASSERT_EQUAL(27, ap_flt_frame_line(0x284, data, 8, line,
                                            sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("284 02 1E 02 1F 0B 7C 4E 20", line);

    /* the parser of the monitor's lines reads it back: same bytes */
    TEST_ASSERT_TRUE(ap_filter_frame(line, strlen(line), 0x284, back,
                                     sizeof(back), &n));
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, back, 8);

    /* a 29-bit id: eight digits */
    TEST_ASSERT_EQUAL(14, ap_flt_frame_line(0x18DAF110, data, 2, line,
                                            sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("18DAF110 02 1E", line);

    /* a frame without data: the id alone */
    TEST_ASSERT_EQUAL(3, ap_flt_frame_line(0x7FF, NULL, 0, line,
                                           sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("7FF", line);

    /* a byte that does not fit is left out whole, never cut in two */
    TEST_ASSERT_EQUAL(9, ap_flt_frame_line(0x284, data, 8, line, 10));
    TEST_ASSERT_EQUAL_STRING("284 02 1E", line);
    TEST_ASSERT_EQUAL(9, ap_flt_frame_line(0x284, data, 8, line, 12));
    TEST_ASSERT_EQUAL_STRING("284 02 1E", line);

    /* no room for the id: nothing, and a terminated string */
    line[0] = 'x';
    TEST_ASSERT_EQUAL(0, ap_flt_frame_line(0x284, data, 8, line, 3));
    TEST_ASSERT_EQUAL_STRING("", line);
}

void test_flt_window_proto(void)
{
    /* the legacy firmware's rule (v4.50 send_can_filter_cmd): the window
       opens on the protocol of the id's width, the sibling at the same bit
       rate when the chip is on the other one */
    TEST_ASSERT_EQUAL_CHAR('7', ap_flt_window_proto('6', true));
    TEST_ASSERT_EQUAL_CHAR('6', ap_flt_window_proto('7', false));
    TEST_ASSERT_EQUAL_CHAR('9', ap_flt_window_proto('8', true));
    TEST_ASSERT_EQUAL_CHAR('8', ap_flt_window_proto('9', false));

    /* the width fits: the chip stays where it is */
    TEST_ASSERT_EQUAL_CHAR('\0', ap_flt_window_proto('6', false));
    TEST_ASSERT_EQUAL_CHAR('\0', ap_flt_window_proto('7', true));
    TEST_ASSERT_EQUAL_CHAR('\0', ap_flt_window_proto('8', false));
    TEST_ASSERT_EQUAL_CHAR('\0', ap_flt_window_proto('9', true));

    /* none of the four: the chip's search, a K-line or J1850 protocol,
       J1939, the user's own CAN definitions, nothing known */
    const char others[] = { '0', '1', '2', '3', '4', '5', 'A', 'B', 'C', '\0' };

    for (size_t i = 0; i < sizeof(others); i++)
    {
        TEST_ASSERT_EQUAL_CHAR('\0', ap_flt_window_proto(others[i], true));
        TEST_ASSERT_EQUAL_CHAR('\0', ap_flt_window_proto(others[i], false));
    }
}

void run_filter_type_tests(void)
{
    RUN_TEST(test_filter_type_from_the_profile_tag);
    RUN_TEST(test_filter_type_follows_its_switch);
    RUN_TEST(test_filter_type_gated_filter_is_never_picked);
    RUN_TEST(test_flt_frame_line);
    RUN_TEST(test_flt_window_proto);
}
