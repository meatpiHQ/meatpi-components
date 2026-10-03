/**
 * @file test_bus_guard.c
 * @brief Host suite for autopid_bus_guard.c: may the OBD chip transmit on a
 *        protocol, given what the native controller heard on the bus? The
 *        cases are the bench's (2026-10-02): the chip's search is safe on a
 *        live bus, a request on a pinned protocol at the other bitrate
 *        drives the sending ECU to bus-off. Run from test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

static const ap_bus_t SILENT = { AP_BUS_SILENT, 0 };
static const ap_bus_t UNKNOWN = { AP_BUS_UNKNOWN, 0 };
static const ap_bus_t LIVE_500 = { AP_BUS_LIVE, 500 };
static const ap_bus_t LIVE_250 = { AP_BUS_LIVE, 250 };
static const ap_bus_t UNREADABLE = { AP_BUS_UNREADABLE, 0 };

void test_guard_proto_bitrates(void)
{
    TEST_ASSERT_EQUAL_UINT16(500, ap_guard_proto_kbps('6'));
    TEST_ASSERT_EQUAL_UINT16(500, ap_guard_proto_kbps('7'));
    TEST_ASSERT_EQUAL_UINT16(250, ap_guard_proto_kbps('8'));
    TEST_ASSERT_EQUAL_UINT16(250, ap_guard_proto_kbps('9'));
    TEST_ASSERT_EQUAL_UINT16(250, ap_guard_proto_kbps('A')); /* J1939 */
    TEST_ASSERT_EQUAL_UINT16(250, ap_guard_proto_kbps('a'));

    /* not judged: the search, K-line / J1850, the user's own CAN */
    TEST_ASSERT_EQUAL_UINT16(0, ap_guard_proto_kbps('0'));
    TEST_ASSERT_EQUAL_UINT16(0, ap_guard_proto_kbps('\0'));
    TEST_ASSERT_EQUAL_UINT16(0, ap_guard_proto_kbps('1'));
    TEST_ASSERT_EQUAL_UINT16(0, ap_guard_proto_kbps('5'));
    TEST_ASSERT_EQUAL_UINT16(0, ap_guard_proto_kbps('B'));
    TEST_ASSERT_EQUAL_UINT16(0, ap_guard_proto_kbps('C'));
}

void test_guard_silent_or_unknown_bus_changes_nothing(void)
{
    /* a gatewayed OBD port is silent until asked: exactly as before */
    const char protos[] = { '0', '3', '6', '7', '8', '9', 'A', 'B' };

    for (size_t i = 0; i < sizeof(protos); i++)
    {
        TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                          ap_guard_decide(&SILENT, protos[i], true));
        TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                          ap_guard_decide(&SILENT, protos[i], false));
        TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                          ap_guard_decide(&UNKNOWN, protos[i], true));
        TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                          ap_guard_decide(NULL, protos[i], true));
    }
}

void test_guard_live_bus_same_bitrate(void)
{
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_500, '6', true));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_500, '7', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_250, '8', true));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_250, '9', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_250, 'A', false));
}

void test_guard_live_bus_other_bitrate(void)
{
    /* the vehicle store's protocol (ours) gives way to the chip's search:
       the device was moved to another vehicle */
    TEST_ASSERT_EQUAL(AP_GUARD_SEARCH,
                      ap_guard_decide(&LIVE_250, '6', false));
    TEST_ASSERT_EQUAL(AP_GUARD_SEARCH,
                      ap_guard_decide(&LIVE_250, '7', false));
    TEST_ASSERT_EQUAL(AP_GUARD_SEARCH,
                      ap_guard_decide(&LIVE_500, '8', false));
    TEST_ASSERT_EQUAL(AP_GUARD_SEARCH,
                      ap_guard_decide(&LIVE_500, '9', false));
    TEST_ASSERT_EQUAL(AP_GUARD_SEARCH,
                      ap_guard_decide(&LIVE_500, 'A', false));

    /* the std_protocol setting (the user's word) is not overruled: the
       chip stays off the bus and the status says why */
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&LIVE_250, '6', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&LIVE_250, '7', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&LIVE_500, '8', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&LIVE_500, '9', true));
}

void test_guard_search_and_non_can_on_a_live_bus(void)
{
    /* the chip's search matches the bus frequency before it sends */
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_250, '0', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_500, '0', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                      ap_guard_decide(&LIVE_500, '\0', false));

    /* K-line / J1850 live on other pins; B and C are user definitions */
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_250, '3', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_500, '5', true));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_250, 'B', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&LIVE_500, 'C', false));
}

void test_guard_unreadable_bus_parks_every_can_transmission(void)
{
    /* traffic at a bitrate neither 250 nor 500 reads: not even the search
       (the chip was only measured on those two) */
    TEST_ASSERT_EQUAL(AP_GUARD_PARK,
                      ap_guard_decide(&UNREADABLE, '6', false));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&UNREADABLE, '9', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK,
                      ap_guard_decide(&UNREADABLE, '0', false));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK,
                      ap_guard_decide(&UNREADABLE, 'A', false));

    /* not CAN, not ours to judge */
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                      ap_guard_decide(&UNREADABLE, '3', false));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW,
                      ap_guard_decide(&UNREADABLE, 'B', false));
}

void test_guard_reason_and_names(void)
{
    char s[176];

    TEST_ASSERT_GREATER_THAN(0, ap_guard_reason(&LIVE_250, '6', true, s,
                                                sizeof(s)));
    TEST_ASSERT_NOT_NULL(strstr(s, "250"));
    TEST_ASSERT_NOT_NULL(strstr(s, "500"));
    TEST_ASSERT_NOT_NULL(strstr(s, "nothing is transmitted"));

    (void)ap_guard_reason(&LIVE_250, '6', false, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, "search"));

    (void)ap_guard_reason(&UNREADABLE, '0', false, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, "could not be read"));

    (void)ap_guard_reason(&LIVE_500, '6', true, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("vehicle bus live at 500 kbit/s", s);

    (void)ap_guard_reason(&SILENT, '6', true, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("vehicle bus silent", s);

    (void)ap_guard_reason(NULL, '6', true, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("vehicle bus not probed", s);

    /* a small buffer is filled, terminated, never overrun */
    char tiny[12];

    memset(tiny, 'x', sizeof(tiny));
    TEST_ASSERT_EQUAL_size_t(sizeof(tiny) - 1,
                             ap_guard_reason(&LIVE_250, '6', true, tiny,
                                             sizeof(tiny)));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[sizeof(tiny) - 1]);
    TEST_ASSERT_EQUAL_size_t(0, ap_guard_reason(&LIVE_250, '6', true, NULL,
                                                0));

    TEST_ASSERT_EQUAL_STRING("allow", ap_guard_verdict_name(AP_GUARD_ALLOW));
    TEST_ASSERT_EQUAL_STRING("search",
                             ap_guard_verdict_name(AP_GUARD_SEARCH));
    TEST_ASSERT_EQUAL_STRING("park", ap_guard_verdict_name(AP_GUARD_PARK));
    TEST_ASSERT_EQUAL_STRING("live", ap_bus_kind_name(AP_BUS_LIVE));
    TEST_ASSERT_EQUAL_STRING("silent", ap_bus_kind_name(AP_BUS_SILENT));
    TEST_ASSERT_EQUAL_STRING("unreadable",
                             ap_bus_kind_name(AP_BUS_UNREADABLE));
    TEST_ASSERT_EQUAL_STRING("unknown", ap_bus_kind_name(AP_BUS_UNKNOWN));
}

void run_bus_guard_tests(void)
{
    RUN_TEST(test_guard_proto_bitrates);
    RUN_TEST(test_guard_silent_or_unknown_bus_changes_nothing);
    RUN_TEST(test_guard_live_bus_same_bitrate);
    RUN_TEST(test_guard_live_bus_other_bitrate);
    RUN_TEST(test_guard_search_and_non_can_on_a_live_bus);
    RUN_TEST(test_guard_unreadable_bus_parks_every_can_transmission);
    RUN_TEST(test_guard_reason_and_names);
}
