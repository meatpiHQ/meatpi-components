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

static const ap_bus_t SILENT = { .kind = AP_BUS_SILENT };
static const ap_bus_t UNKNOWN = { .kind = AP_BUS_UNKNOWN };
static const ap_bus_t LIVE_500 = { .kind = AP_BUS_LIVE, .kbps = 500 };
static const ap_bus_t LIVE_250 = { .kind = AP_BUS_LIVE, .kbps = 250 };
static const ap_bus_t UNREADABLE = { .kind = AP_BUS_UNREADABLE };

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

/* where the chip was let talk: the mark of a protocol */
void test_guard_sent_marks(void)
{
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_500, ap_guard_sent_mark('6'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_500, ap_guard_sent_mark('7'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_250, ap_guard_sent_mark('8'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_250, ap_guard_sent_mark('9'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_250, ap_guard_sent_mark('A'));

    /* the chip's search and the user's own CAN may talk at either */
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_500 | AP_SENT_250,
                            ap_guard_sent_mark('0'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_500 | AP_SENT_250,
                            ap_guard_sent_mark('\0'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_500 | AP_SENT_250,
                            ap_guard_sent_mark('B'));
    TEST_ASSERT_EQUAL_UINT8(AP_SENT_500 | AP_SENT_250,
                            ap_guard_sent_mark('C'));

    /* SAE J1850 and the K-line are other pins */
    for (char p = '1'; p <= '5'; p++)
    {
        TEST_ASSERT_EQUAL_UINT8(0, ap_guard_sent_mark(p));
    }
}

/* A unit with its own CAN node on (bench 2026-11-13): the node acknowledges
   the OBD chip's requests and reads them as frames, so a bus with nobody on
   it read "live at 500 kbit/s" for the boot and a 250 kbit/s car that woke
   up later was never asked; and it took the chip's talk on 250 kbit/s for
   traffic it cannot read. Which pictures have to be held against the quiet. */
void test_guard_a_running_nodes_picture_may_be_the_chips_own(void)
{
    /* the chip has not talked: what the node read named the bus */
    TEST_ASSERT_FALSE(ap_guard_own_doing(&LIVE_500, 0));
    TEST_ASSERT_FALSE(ap_guard_own_doing(&LIVE_250, 0));
    TEST_ASSERT_FALSE(ap_guard_own_doing(&UNREADABLE, 0));

    /* frames at the bit rate the chip talked at may be its own */
    TEST_ASSERT_TRUE(ap_guard_own_doing(&LIVE_500, ap_guard_sent_mark('6')));
    TEST_ASSERT_TRUE(ap_guard_own_doing(&LIVE_250, ap_guard_sent_mark('8')));
    TEST_ASSERT_TRUE(ap_guard_own_doing(&LIVE_500, ap_guard_sent_mark('0')));
    TEST_ASSERT_TRUE(ap_guard_own_doing(&LIVE_250, ap_guard_sent_mark('0')));

    /* frames at the OTHER bit rate are not: a bus that is live there named
       itself, and a pinned request stays off it as before */
    TEST_ASSERT_FALSE(ap_guard_own_doing(&LIVE_500, ap_guard_sent_mark('8')));
    TEST_ASSERT_FALSE(ap_guard_own_doing(&LIVE_250, ap_guard_sent_mark('6')));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&LIVE_500, '8', true));

    /* traffic the node cannot read may be the chip's talk at its other bit
       rate, wherever it talked on CAN; not when it only used other pins */
    TEST_ASSERT_TRUE(ap_guard_own_doing(&UNREADABLE, ap_guard_sent_mark('8')));
    TEST_ASSERT_TRUE(ap_guard_own_doing(&UNREADABLE, ap_guard_sent_mark('6')));
    TEST_ASSERT_FALSE(ap_guard_own_doing(&UNREADABLE,
                                         ap_guard_sent_mark('3')));

    /* nothing to explain away */
    TEST_ASSERT_FALSE(ap_guard_own_doing(&SILENT, AP_SENT_500 | AP_SENT_250));
    TEST_ASSERT_FALSE(ap_guard_own_doing(&UNKNOWN,
                                         AP_SENT_500 | AP_SENT_250));
    TEST_ASSERT_FALSE(ap_guard_own_doing(NULL, AP_SENT_500 | AP_SENT_250));

    /* the walk that failed: 6 and 7 tried on a bus with nobody on it, then
       8. The picture is the chip's own, the quiet says so, the bus is silent
       and 8 is asked */
    uint8_t sent = ap_guard_sent_mark('6') | ap_guard_sent_mark('7');

    TEST_ASSERT_TRUE(ap_guard_own_doing(&LIVE_500, sent));
    TEST_ASSERT_EQUAL(AP_GUARD_ALLOW, ap_guard_decide(&SILENT, '8', true));
}

/* The sentence of a park on traffic that could not be read. The guard's own
   listener tried 250 and 500 kbit/s and says so. A CAN node of the unit's
   own that is fixed at one bit rate tried that one alone: with the node at
   500 on a 250 kbit/s bus the sentence said "not 250 or 500 kbit/s" (bench
   2026-11-13). It names the node's bit rate and its setting; the verdict is
   the same. */
void test_guard_a_fixed_nodes_unreadable_names_its_setting(void)
{
    const ap_bus_t by_node = { AP_BUS_UNREADABLE, 0, 500 };
    const ap_bus_t at_1000 = { AP_BUS_UNREADABLE, 0, 1000 };
    const ap_bus_t live = { AP_BUS_LIVE, 250, 500 };
    char s[176]; /* the size of the buffers that keep the sentence */

    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&by_node, '6', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&by_node, '8', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&by_node, '0', false));

    size_t n = ap_guard_reason(&by_node, '0', false, s, sizeof(s));

    TEST_ASSERT_EQUAL_STRING("the vehicle bus carries traffic that WiCAN's "
                             "CAN controller, set to 500 kbit/s, cannot "
                             "read: nothing is transmitted (set the CAN bit "
                             "rate to Automatic)", s);
    TEST_ASSERT_EQUAL_UINT(strlen(s), n);
    TEST_ASSERT_NULL(strstr(s, "not 250"));

    /* whole in that buffer at the longest bit rate of the setting */
    n = ap_guard_reason(&at_1000, '6', true, s, sizeof(s));
    TEST_ASSERT_TRUE(n < sizeof(s) - 1);
    TEST_ASSERT_NOT_NULL(strstr(s, "set to 1000 kbit/s"));
    TEST_ASSERT_NOT_NULL(strstr(s, "(set the CAN bit rate to Automatic)"));

    /* the listener's word, and a node on "auto" (no bit rate of its own) */
    (void)ap_guard_reason(&UNREADABLE, '0', false, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, "could not be read (not 250 or 500 "
                                   "kbit/s)"));
    TEST_ASSERT_NULL(strstr(s, "CAN controller"));

    /* a chain of the tables on such a bus */
    (void)ap_guard_chain_reason(&by_node, '6', "the init of row Soc", s,
                                sizeof(s));
    TEST_ASSERT_EQUAL_STRING("the init of row Soc sets protocol 6 and the "
                             "vehicle bus carries traffic that WiCAN's CAN "
                             "controller, set to 500 kbit/s, cannot read: "
                             "not sent", s);

    /* only "unreadable" speaks of the node */
    (void)ap_guard_reason(&live, '6', true, s, sizeof(s));
    TEST_ASSERT_NULL(strstr(s, "CAN controller"));
    TEST_ASSERT_NOT_NULL(strstr(s, "runs at 250 kbit/s"));

    /* the quiet makes "unreadable" of a false "live" and names no node:
       that is the glue's to add (it knows who runs) */
    ap_bus_t bus = LIVE_500;

    ap_guard_after_quiet(&bus, AP_SENT_500, AP_HEARD_NOISE);
    TEST_ASSERT_EQUAL(AP_BUS_UNREADABLE, bus.kind);
    TEST_ASSERT_EQUAL_UINT16(0, bus.node_kbps);
}

/* What the quiet makes of such a picture (bench 2026-11-13). Nothing heard:
   silent but for us. Frames the node read with the chip quiet: somebody at
   its bit rate, "live" stands. Noise: somebody who does not talk at the bit
   rate of the frames read before (the chip's own), so "live" must not
   become the boot's word: a 250 kbit/s frame fell into the quiet of a "live
   at 500", and the chip would have talked at 500 for good. */
void test_guard_a_picture_held_against_the_quiet(void)
{
    const uint8_t both = AP_SENT_500 | AP_SENT_250;
    ap_bus_t bus;

    /* "live" at a bit rate the chip talked at */
    bus = LIVE_500;
    ap_guard_after_quiet(&bus, AP_SENT_500, AP_HEARD_NOTHING);
    TEST_ASSERT_EQUAL(AP_BUS_SILENT, bus.kind);
    TEST_ASSERT_EQUAL_UINT16(0, bus.kbps);

    bus = LIVE_500;
    ap_guard_after_quiet(&bus, AP_SENT_500, AP_HEARD_FRAMES);
    TEST_ASSERT_EQUAL(AP_BUS_LIVE, bus.kind);
    TEST_ASSERT_EQUAL_UINT16(500, bus.kbps);

    bus = LIVE_500;
    ap_guard_after_quiet(&bus, AP_SENT_500, AP_HEARD_NOISE);
    TEST_ASSERT_EQUAL(AP_BUS_UNREADABLE, bus.kind);
    TEST_ASSERT_EQUAL_UINT16(0, bus.kbps);
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&bus, '6', true));
    TEST_ASSERT_EQUAL(AP_GUARD_PARK, ap_guard_decide(&bus, '0', false));

    /* "unreadable" once the chip talked on CAN */
    bus = UNREADABLE;
    ap_guard_after_quiet(&bus, both, AP_HEARD_NOTHING);
    TEST_ASSERT_EQUAL(AP_BUS_SILENT, bus.kind);

    bus = UNREADABLE;
    ap_guard_after_quiet(&bus, both, AP_HEARD_NOISE);
    TEST_ASSERT_EQUAL(AP_BUS_UNREADABLE, bus.kind);

    bus = UNREADABLE;       /* frames: the next look reads them as live */
    ap_guard_after_quiet(&bus, both, AP_HEARD_FRAMES);
    TEST_ASSERT_EQUAL(AP_BUS_UNREADABLE, bus.kind);

    /* a picture that is not the chip's doing stands, whatever the quiet:
       frames at the other bit rate, anything before the chip talked */
    bus = LIVE_250;
    ap_guard_after_quiet(&bus, AP_SENT_500, AP_HEARD_NOTHING);
    TEST_ASSERT_EQUAL(AP_BUS_LIVE, bus.kind);
    TEST_ASSERT_EQUAL_UINT16(250, bus.kbps);

    bus = LIVE_500;
    ap_guard_after_quiet(&bus, 0, AP_HEARD_NOISE);
    TEST_ASSERT_EQUAL(AP_BUS_LIVE, bus.kind);

    bus = UNREADABLE;
    ap_guard_after_quiet(&bus, 0, AP_HEARD_NOTHING);
    TEST_ASSERT_EQUAL(AP_BUS_UNREADABLE, bus.kind);

    bus = SILENT;
    ap_guard_after_quiet(&bus, both, AP_HEARD_NOISE);
    TEST_ASSERT_EQUAL(AP_BUS_SILENT, bus.kind);

    ap_guard_after_quiet(NULL, both, AP_HEARD_NOISE);   /* no crash */
}

/* The quiet is a look of 300 ms, and sparse traffic falls between two of
   them (bench 2026-11-13: two frames a second at 250 kbit/s, the guard
   parked, let go, parked again, and the chip talked in between). Who was
   heard with the chip quiet stays heard for ten seconds. */
void test_guard_who_was_heard_stays_heard_for_a_while(void)
{
    const int64_t s = 1000 * 1000;

    /* nobody was ever heard */
    TEST_ASSERT_FALSE(ap_guard_heard_stands(5 * s, 0));
    TEST_ASSERT_FALSE(ap_guard_heard_stands(0, 0));

    /* heard just now, and through the ten seconds behind it */
    TEST_ASSERT_TRUE(ap_guard_heard_stands(100 * s, 100 * s));
    TEST_ASSERT_TRUE(ap_guard_heard_stands(100 * s + 1, 100 * s));
    TEST_ASSERT_TRUE(ap_guard_heard_stands(109 * s, 100 * s));
    TEST_ASSERT_TRUE(ap_guard_heard_stands(
        100 * s + AP_GUARD_HEARD_HOLD_US - 1, 100 * s));

    /* then the bus is asked again */
    TEST_ASSERT_FALSE(ap_guard_heard_stands(
        100 * s + AP_GUARD_HEARD_HOLD_US, 100 * s));
    TEST_ASSERT_FALSE(ap_guard_heard_stands(200 * s, 100 * s));

    /* a clock that reads earlier than the hearing explains nothing */
    TEST_ASSERT_FALSE(ap_guard_heard_stands(99 * s, 100 * s));
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

void test_guard_a_verdict_covers_one_protocol(void)
{
    /* the bench case of 2026-10-05: the truck's verdict (protocol 9, 250
       kbit/s) stands, the 500 kbit/s car is made current, and the next
       prelude would pin 6 */
    TEST_ASSERT_TRUE(ap_guard_pin_allowed('9', '9'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('6', '9'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('7', '9'));

    /* same bit rate, another protocol: still another look (the verdict was
       not taken for it) */
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('8', '9'));

    /* the search counts as a protocol of its own, both ways: allowed after
       a SEARCH verdict, not on the strength of a pinned protocol's */
    TEST_ASSERT_TRUE(ap_guard_pin_allowed('0', '0'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('0', '6'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('6', '0'));

    /* no verdict stands (the guard was re-armed, or it parked the chip) */
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('6', '\0'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('0', '\0'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('\0', '\0'));
    TEST_ASSERT_FALSE(ap_guard_pin_allowed('A', '\0'));

    /* K-line / J1850 and the user's own CAN definitions are not this
       guard's, with or without a verdict */
    TEST_ASSERT_TRUE(ap_guard_pin_allowed('3', '\0'));
    TEST_ASSERT_TRUE(ap_guard_pin_allowed('5', '9'));
    TEST_ASSERT_TRUE(ap_guard_pin_allowed('B', '6'));
    TEST_ASSERT_TRUE(ap_guard_pin_allowed('C', '\0'));
}

void test_guard_cmd_proto(void)
{
    /* the forms the chip takes: any case, spaces anywhere */
    TEST_ASSERT_EQUAL_CHAR('6', ap_guard_cmd_proto("ATSP6", 5));
    TEST_ASSERT_EQUAL_CHAR('6', ap_guard_cmd_proto("ATTP6", 5));
    TEST_ASSERT_EQUAL_CHAR('7', ap_guard_cmd_proto("at sp 7", 7));
    TEST_ASSERT_EQUAL_CHAR('9', ap_guard_cmd_proto(" AT TP 9\r", 9));
    TEST_ASSERT_EQUAL_CHAR('A', ap_guard_cmd_proto("ATTPA", 5));
    TEST_ASSERT_EQUAL_CHAR('B', ap_guard_cmd_proto("ATSPB", 5));
    TEST_ASSERT_EQUAL_CHAR('0', ap_guard_cmd_proto("ATSP0", 5));

    /* "A" before the digit: that protocol first, the search behind it */
    TEST_ASSERT_EQUAL_CHAR('6', ap_guard_cmd_proto("ATSPA6", 6));
    TEST_ASSERT_EQUAL_CHAR('8', ap_guard_cmd_proto("attpa8", 6));
    TEST_ASSERT_EQUAL_CHAR('A', ap_guard_cmd_proto("ATSPAA", 6));
    TEST_ASSERT_EQUAL_CHAR('0', ap_guard_cmd_proto("ATSP00", 6));

    /* only the length given is read */
    TEST_ASSERT_EQUAL_CHAR('6', ap_guard_cmd_proto("ATSP6;ATSH7DF", 5));
    TEST_ASSERT_EQUAL_CHAR('\0', ap_guard_cmd_proto("ATSP6;ATSH7DF", 13));

    /* everything else sets no protocol */
    const char *none[] = { "ATSH7DF", "ATST96", "ATS1", "ATH0", "ATCRA7E8",
                           "ATSP", "ATTP", "ATSPD", "ATSP60", "ATSP123",
                           "ATZ", "010C", "22F40C", "22 F4 0C", "ATPB E0 04",
                           "ATTA 6", "ATPPS", "xATSP6", "" };

    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++)
    {
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0',
                                       ap_guard_cmd_proto(none[i],
                                                          strlen(none[i])),
                                       none[i]);
    }

    TEST_ASSERT_EQUAL_CHAR('\0', ap_guard_cmd_proto(NULL, 5));
}

void test_guard_chain_of_the_tables(void)
{
    char last = 'x', refused = 'x';
    char s[176];

    /* the bench case of 2026-10-05: a 500 kbit/s profile's chain on a 250
       kbit/s truck ... */
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&LIVE_250, "ATSP6;ATSH7DF",
                                             &last, &refused));
    TEST_ASSERT_EQUAL_CHAR('6', last);
    TEST_ASSERT_EQUAL_CHAR('6', refused);

    /* ... and in the car it was written for */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_500, "ATSP6;ATSH7DF",
                                            &last, &refused));
    TEST_ASSERT_EQUAL_CHAR('6', last);
    TEST_ASSERT_EQUAL_CHAR('\0', refused);

    /* same bit rate, another protocol: a profile that mixes 11 and 29 bit */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(
        &LIVE_500, "ATTP6;ATSH7DF;ATCRA;ATTP7;ATSH17FC007B", &last,
        &refused));
    TEST_ASSERT_EQUAL_CHAR('7', last);

    /* every protocol of the chain counts, not the last one (a request may
       sit between two of them) */
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&LIVE_250, "ATTP6;1003;ATTP9",
                                             &last, &refused));
    TEST_ASSERT_EQUAL_CHAR('9', last);
    TEST_ASSERT_EQUAL_CHAR('6', refused);

    /* a chain that sets none: nothing to rule on, whatever the bus */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250,
                                            "ATSH7E4;ATCRA7EC;ATFCSH7E4",
                                            &last, &refused));
    TEST_ASSERT_EQUAL_CHAR('\0', last);
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&UNREADABLE, "ATSH7E4", &last,
                                            NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, "", &last, &refused));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, NULL, &last,
                                            &refused));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, "010C", NULL, NULL));

    /* a silent or unknown bus: as before the guard */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&SILENT, "ATSP6", &last,
                                            &refused));
    TEST_ASSERT_EQUAL_CHAR('6', last);
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&UNKNOWN, "ATSP8", NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(NULL, "ATSP8", &last, NULL));
    TEST_ASSERT_EQUAL_CHAR('8', last);

    /* an unreadable bus takes no CAN protocol, not even the search */
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&UNREADABLE, "ATSP6", NULL,
                                             &refused));
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&UNREADABLE, "ATSP0", NULL,
                                             &refused));
    TEST_ASSERT_EQUAL_CHAR('0', refused);

    /* the chip's search is safe on a live bus; K-line / J1850 and the
       user's own CAN definitions are not this guard's */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, "ATSP0", NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, "ATSP3;ATTPB", &last,
                                            NULL));
    TEST_ASSERT_EQUAL_CHAR('B', last);

    /* empty elements, a trailing separator, the search-behind form */
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&LIVE_500,
                                             ";;ATH1;;at sp a9;", &last,
                                             &refused));
    TEST_ASSERT_EQUAL_CHAR('9', refused);

    /* the sentence */
    TEST_ASSERT_GREATER_THAN(0, ap_guard_chain_reason(&LIVE_250, '6',
                                                      "the init of row Soc",
                                                      s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("the init of row Soc sets protocol 6 (500 "
                             "kbit/s) and the vehicle bus runs at 250 "
                             "kbit/s: not sent", s);

    (void)ap_guard_chain_reason(&UNREADABLE, '0', "dtc_init", s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, "dtc_init sets protocol 0"));
    TEST_ASSERT_NOT_NULL(strstr(s, "could not be read"));

    (void)ap_guard_chain_reason(&LIVE_500, '9', NULL, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, "an init sets protocol 9 (250 kbit/s)"));

    char tiny[12];

    memset(tiny, 'x', sizeof(tiny));
    TEST_ASSERT_EQUAL_size_t(sizeof(tiny) - 1,
                             ap_guard_chain_reason(&LIVE_250, '6', "row X",
                                                   tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[sizeof(tiny) - 1]);
    TEST_ASSERT_EQUAL_size_t(0, ap_guard_chain_reason(&LIVE_250, '6',
                                                      "row X", NULL, 0));
}

void test_guard_reset_in_a_chain(void)
{
    char last = 'x', refused = 'x';

    TEST_ASSERT_TRUE(ap_guard_cmd_resets("ATZ", 3));
    TEST_ASSERT_TRUE(ap_guard_cmd_resets("at z", 4));
    TEST_ASSERT_TRUE(ap_guard_cmd_resets("ATD", 3));
    TEST_ASSERT_TRUE(ap_guard_cmd_resets(" AT WS\r", 7));
    TEST_ASSERT_TRUE(ap_guard_cmd_resets("ATZ;ATSP6", 3));

    const char *no[] = { "ATDP", "ATDPN", "ATD0", "ATD1", "ATZZ", "ATWM",
                         "ATW", "ATSP6", "ATTP0", "010C", "AT", "" };

    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
    {
        TEST_ASSERT_FALSE_MESSAGE(ap_guard_cmd_resets(no[i], strlen(no[i])),
                                  no[i]);
    }

    TEST_ASSERT_FALSE(ap_guard_cmd_resets(NULL, 3));

    /* the sender puts the baseline prelude behind a reset: what the chain
       set before it is gone, what it sets after it stands */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_500, "ATSP7;ATZ;ATSH7E0",
                                            &last, NULL));
    TEST_ASSERT_EQUAL_CHAR('\0', last);
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_500, "ATZ;ATSP7", &last,
                                            NULL));
    TEST_ASSERT_EQUAL_CHAR('7', last);

    /* ... and a reset launders nothing: the protocol before it was set */
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&LIVE_250, "ATSP6;ATZ", &last,
                                             &refused));
    TEST_ASSERT_EQUAL_CHAR('6', refused);
    TEST_ASSERT_EQUAL_CHAR('\0', last);
}

void test_guard_the_profile_probes_chains(void)
{
    /* the detection job's profile probe asks the guard for the prelude of
       the profile's protocol as it is (2026-11-10): a prelude names its
       protocol (ATTP<p>) and is judged like any chain of the tables. Bench:
       a car profile on protocol 6 named in the Quick Setup on a 250 kbit/s
       bus that answers nothing standard put 865 error frames on that bus
       in 0.18 s; the probe had not asked. */
    char refused = 'x';
    char s[176];

    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&LIVE_250,
                                             ap_veh_prelude_for('6'), NULL,
                                             &refused));
    TEST_ASSERT_EQUAL_CHAR('6', refused);
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&LIVE_250,
                                             ap_veh_prelude_for('7'), NULL,
                                             &refused));
    TEST_ASSERT_EQUAL_CHAR('7', refused);
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(&UNREADABLE,
                                             ap_veh_prelude_for('6'), NULL,
                                             NULL));

    /* the bus's own protocols, a silent bus (a gatewayed port answers only
       when asked), K-line and J1850 (other pins): sent as before */
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, ap_veh_prelude_for('8'),
                                            NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, ap_veh_prelude_for('9'),
                                            NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_500, ap_veh_prelude_for('6'),
                                            NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&SILENT, ap_veh_prelude_for('6'),
                                            NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, ap_veh_prelude_for('3'),
                                            NULL, NULL));
    TEST_ASSERT_TRUE(ap_guard_chain_allowed(&LIVE_250, ap_veh_prelude_for('1'),
                                            NULL, NULL));

    /* a request's own init of the published MEB profile on that bus */
    TEST_ASSERT_FALSE(ap_guard_chain_allowed(
        &LIVE_250, "ATSP7;ATCP17;ATSHFC007B;ATFCSH17FC007B;ATCRA17FE007B;",
        NULL, &refused));
    TEST_ASSERT_EQUAL_CHAR('7', refused);

    /* the sentence the job hands on in place of "no ECU response" */
    (void)ap_guard_chain_reason(&LIVE_250, '6', "the profile", s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("the profile sets protocol 6 (500 kbit/s) and "
                             "the vehicle bus runs at 250 kbit/s: not sent",
                             s);
}

void run_bus_guard_tests(void)
{
    RUN_TEST(test_guard_reset_in_a_chain);
    RUN_TEST(test_guard_cmd_proto);
    RUN_TEST(test_guard_chain_of_the_tables);
    RUN_TEST(test_guard_the_profile_probes_chains);
    RUN_TEST(test_guard_a_verdict_covers_one_protocol);
    RUN_TEST(test_guard_proto_bitrates);
    RUN_TEST(test_guard_sent_marks);
    RUN_TEST(test_guard_a_running_nodes_picture_may_be_the_chips_own);
    RUN_TEST(test_guard_a_picture_held_against_the_quiet);
    RUN_TEST(test_guard_who_was_heard_stays_heard_for_a_while);
    RUN_TEST(test_guard_a_fixed_nodes_unreadable_names_its_setting);
    RUN_TEST(test_guard_silent_or_unknown_bus_changes_nothing);
    RUN_TEST(test_guard_live_bus_same_bitrate);
    RUN_TEST(test_guard_live_bus_other_bitrate);
    RUN_TEST(test_guard_search_and_non_can_on_a_live_bus);
    RUN_TEST(test_guard_unreadable_bus_parks_every_can_transmission);
    RUN_TEST(test_guard_reason_and_names);
}
