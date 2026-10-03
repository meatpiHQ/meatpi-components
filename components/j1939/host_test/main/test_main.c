/**
 * @file test_main.c
 * @brief Host suite for the pure half of the j1939 component. This file:
 *        the 29-bit identifier, frame kinds, the "is this a J1939 bus"
 *        evidence and the VIN message (j1939_core.c). The identifiers and
 *        payloads are the ones tools/testbench/lib/j1939_ref.py produces
 *        (the bench's independent reference).
 */
#include <string.h>

#include "unity.h"

#include "j1939_core.h"

/* test_tp.c: transport protocol; test_values.c: DM1 / DM2 and the value
 * table; test_cache.c: the message store and the sources; test_active.c:
 * the address claim, request / acknowledgment, transport as destination */
void run_tp_tests(void);
void run_values_tests(void);
void run_cache_tests(void);
void run_active_tests(void);

/* ---- identifier ------------------------------------------------------------- */

void test_id_parse_pdu2_broadcast(void)
{
    j1939_id_t id;

    /* EEC1 from the engine: priority 3, group F004, source 00 */
    j1939_id_parse(0x0CF00400u, &id);
    TEST_ASSERT_EQUAL_UINT8(3, id.prio);
    TEST_ASSERT_EQUAL_HEX32(0x00F004u, id.pgn);
    TEST_ASSERT_EQUAL_HEX8(0x00, id.sa);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_GLOBAL, id.da);

    /* CCVS1: priority 6 */
    j1939_id_parse(0x18FEF100u, &id);
    TEST_ASSERT_EQUAL_UINT8(6, id.prio);
    TEST_ASSERT_EQUAL_HEX32(0x00FEF1u, id.pgn);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_GLOBAL, id.da);
}

void test_id_parse_pdu1_destination(void)
{
    j1939_id_t id;

    /* a request sent by F9 to the engine: the PS byte is the destination,
       not part of the group number */
    j1939_id_parse(0x18EA00F9u, &id);
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_REQUEST, id.pgn);
    TEST_ASSERT_EQUAL_HEX8(0xF9, id.sa);
    TEST_ASSERT_EQUAL_HEX8(0x00, id.da);

    /* the same request to everyone */
    j1939_id_parse(0x18EAFFF9u, &id);
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_REQUEST, id.pgn);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_GLOBAL, id.da);

    /* a BAM announce: TP.CM to FF, priority 7 */
    j1939_id_parse(0x1CECFF00u, &id);
    TEST_ASSERT_EQUAL_UINT8(7, id.prio);
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_TP_CM, id.pgn);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_GLOBAL, id.da);
}

void test_id_make_round_trip(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x0CF00400u, j1939_id_make(3, 0x00F004u, 0x00,
                                                       J1939_ADDR_GLOBAL));
    TEST_ASSERT_EQUAL_HEX32(0x18EA00F9u,
                            j1939_id_make(6, J1939_PGN_REQUEST, 0xF9, 0x00));
    TEST_ASSERT_EQUAL_HEX32(0x18EAFFF9u,
                            j1939_id_make(6, J1939_PGN_REQUEST, 0xF9,
                                          J1939_ADDR_GLOBAL));
    /* the destination of a PDU2 group is ignored */
    TEST_ASSERT_EQUAL_HEX32(0x18FEF100u, j1939_id_make(6, 0x00FEF1u, 0x00,
                                                       0x33));
    /* the extended data page bit never comes out */
    TEST_ASSERT_EQUAL_HEX32(0x18FEF100u, j1939_id_make(6, 0x02FEF1u, 0x00,
                                                       0xFF));
    TEST_ASSERT_TRUE(j1939_pgn_is_pdu1(J1939_PGN_REQUEST));
    TEST_ASSERT_TRUE(j1939_pgn_is_pdu1(0x00EF00u));
    TEST_ASSERT_FALSE(j1939_pgn_is_pdu1(0x00F004u));
    TEST_ASSERT_FALSE(j1939_pgn_is_pdu1(0x00FECAu));
}

/* ---- frame kinds ---------------------------------------------------------------- */

void test_classify_kinds(void)
{
    j1939_id_t id;

    TEST_ASSERT_EQUAL(J1939_KIND_DATA,
                      j1939_classify(0x0CF00400u, true, false, &id));
    TEST_ASSERT_EQUAL_HEX32(0x00F004u, id.pgn);

    TEST_ASSERT_EQUAL(J1939_KIND_TP_CM,
                      j1939_classify(0x1CECFF00u, true, false, &id));
    TEST_ASSERT_EQUAL(J1939_KIND_TP_DT,
                      j1939_classify(0x1CEBFF00u, true, false, &id));
    TEST_ASSERT_EQUAL_HEX8(0x00, id.sa);

    /* a request, an acknowledgment, an address claim: ordinary groups */
    TEST_ASSERT_EQUAL(J1939_KIND_DATA,
                      j1939_classify(0x18EA00F9u, true, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_DATA,
                      j1939_classify(0x18E8FF00u, true, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_DATA,
                      j1939_classify(0x18EEFF00u, true, false, NULL));
}

void test_classify_diag_and_foreign(void)
{
    j1939_id_t id;

    /* OBD / UDS on 29-bit identifiers: somebody else's conversation */
    TEST_ASSERT_EQUAL(J1939_KIND_DIAG,
                      j1939_classify(0x18DB33F1u, true, false, &id));
    TEST_ASSERT_EQUAL_HEX8(0x33, id.da);
    TEST_ASSERT_EQUAL(J1939_KIND_DIAG,
                      j1939_classify(0x18DAF110u, true, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_DIAG,
                      j1939_classify(0x18CE00F1u, true, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_DIAG,
                      j1939_classify(0x18CD33F1u, true, false, NULL));

    /* 11-bit and remote frames */
    TEST_ASSERT_EQUAL(J1939_KIND_FOREIGN,
                      j1939_classify(0x7E8u, false, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_FOREIGN,
                      j1939_classify(0x0CF00400u, true, true, NULL));

    /* the extended data page bit: 29-bit identifiers of passenger cars
       (seen on VAG buses) that no J1939 node sends */
    TEST_ASSERT_EQUAL(J1939_KIND_FOREIGN,
                      j1939_classify(0x17F00010u, true, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_FOREIGN,
                      j1939_classify(0x1B000010u, true, false, NULL));
    TEST_ASSERT_EQUAL(J1939_KIND_FOREIGN,
                      j1939_classify(0x12DD5501u, true, false, NULL));
}

/* ---- bus evidence ---------------------------------------------------------------- */

void test_bus_two_groups_make_j1939(void)
{
    j1939_bus_evidence_t ev = { 0 };

    TEST_ASSERT_EQUAL(J1939_BUS_UNKNOWN, j1939_bus_verdict(&ev));

    /* one well-known group, however often, is not enough */
    for (int i = 0; i < 200; i++)
    {
        j1939_bus_note(&ev, 0x0CF00400u, true, false, 8);
    }

    TEST_ASSERT_EQUAL(J1939_BUS_UNKNOWN, j1939_bus_verdict(&ev));
    TEST_ASSERT_EQUAL_UINT32(200, ev.known);

    /* a second one from another controller settles it */
    j1939_bus_note(&ev, 0x18FEF117u, true, false, 8);
    TEST_ASSERT_EQUAL(J1939_BUS_J1939, j1939_bus_verdict(&ev));
    TEST_ASSERT_EQUAL_STRING("j1939", j1939_bus_name(J1939_BUS_J1939));
}

void test_bus_known_group_needs_eight_bytes(void)
{
    j1939_bus_evidence_t ev = { 0 };

    j1939_bus_note(&ev, 0x0CF00400u, true, false, 3);
    j1939_bus_note(&ev, 0x18FEF100u, true, false, 2);
    TEST_ASSERT_EQUAL_UINT32(0, ev.known);
    TEST_ASSERT_EQUAL(J1939_BUS_UNKNOWN, j1939_bus_verdict(&ev));
}

void test_bus_other_traffic(void)
{
    j1939_bus_evidence_t ev = { 0 };

    /* an OBD-II car on 29-bit identifiers, and its 11-bit body traffic */
    for (int i = 0; i < 30; i++)
    {
        j1939_bus_note(&ev, 0x18DAF110u, true, false, 8);
        j1939_bus_note(&ev, 0x3D0u, false, false, 8);
    }

    TEST_ASSERT_EQUAL(J1939_BUS_OTHER, j1939_bus_verdict(&ev));
    TEST_ASSERT_EQUAL_UINT32(30, ev.diag);
    TEST_ASSERT_EQUAL_UINT32(30, ev.foreign);
    TEST_ASSERT_EQUAL_STRING("other", j1939_bus_name(J1939_BUS_OTHER));

    /* a truck that also carries diagnostics is still a J1939 bus */
    j1939_bus_note(&ev, 0x0CF00400u, true, false, 8);
    j1939_bus_note(&ev, 0x18FEEE00u, true, false, 8);
    TEST_ASSERT_EQUAL(J1939_BUS_J1939, j1939_bus_verdict(&ev));
}

void test_bus_few_frames_stay_unknown(void)
{
    j1939_bus_evidence_t ev = { 0 };

    for (unsigned i = 0; i < J1939_BUS_OTHER_AFTER - 1u; i++)
    {
        j1939_bus_note(&ev, 0x18FF1021u, true, false, 8); /* proprietary */
    }

    TEST_ASSERT_EQUAL(J1939_BUS_UNKNOWN, j1939_bus_verdict(&ev));
    j1939_bus_note(&ev, 0x18FF1021u, true, false, 8);
    TEST_ASSERT_EQUAL(J1939_BUS_OTHER, j1939_bus_verdict(&ev));
    TEST_ASSERT_EQUAL_STRING("unknown", j1939_bus_name(J1939_BUS_UNKNOWN));
}

/* ---- VIN -------------------------------------------------------------------------- */

void test_vin_with_delimiter(void)
{
    /* the bench truck's message: 17 characters and '*' */
    static const uint8_t MSG[] =
    {
        0x31, 0x57, 0x43, 0x41, 0x4E, 0x4A, 0x31, 0x39, 0x33,
        0x39, 0x54, 0x52, 0x55, 0x43, 0x4B, 0x30, 0x31, 0x2A,
    };
    char vin[J1939_VIN_LEN + 1] = "";

    TEST_ASSERT_TRUE(j1939_vin_parse(MSG, sizeof(MSG), vin));
    TEST_ASSERT_EQUAL_STRING("1WCANJ1939TRUCK01", vin);
}

void test_vin_without_delimiter_and_padded(void)
{
    char vin[J1939_VIN_LEN + 1] = "";
    uint8_t msg[24];

    memcpy(msg, "1WCANJ1939TRUCK01", 17);
    TEST_ASSERT_TRUE(j1939_vin_parse(msg, 17, vin));
    TEST_ASSERT_EQUAL_STRING("1WCANJ1939TRUCK01", vin);

    /* transport padding behind the characters */
    memset(msg + 17, 0xFF, 4);
    vin[0] = '\0';
    TEST_ASSERT_TRUE(j1939_vin_parse(msg, 21, vin));
    TEST_ASSERT_EQUAL_STRING("1WCANJ1939TRUCK01", vin);

    /* spaces before the delimiter */
    memcpy(msg + 17, "  *", 3);
    vin[0] = '\0';
    TEST_ASSERT_TRUE(j1939_vin_parse(msg, 20, vin));
    TEST_ASSERT_EQUAL_STRING("1WCANJ1939TRUCK01", vin);
}

void test_vin_rejects_what_is_not_a_vin(void)
{
    char vin[J1939_VIN_LEN + 1] = "untouched";
    uint8_t msg[24];

    /* too short, too long */
    memcpy(msg, "1WCANJ1939TRUCK0*", 17);
    TEST_ASSERT_FALSE(j1939_vin_parse(msg, 17, vin));
    memcpy(msg, "1WCANJ1939TRUCK012*", 19);
    TEST_ASSERT_FALSE(j1939_vin_parse(msg, 19, vin));

    /* letters a VIN never has, lower case, an empty message */
    memcpy(msg, "1WCANJ1939TRUCKO1*", 18);
    TEST_ASSERT_FALSE(j1939_vin_parse(msg, 18, vin));
    memcpy(msg, "1wcanj1939truck01*", 18);
    TEST_ASSERT_FALSE(j1939_vin_parse(msg, 18, vin));
    TEST_ASSERT_FALSE(j1939_vin_parse(msg, 0, vin));
    memset(msg, 0xFF, sizeof(msg));
    TEST_ASSERT_FALSE(j1939_vin_parse(msg, 18, vin));
    TEST_ASSERT_FALSE(j1939_vin_parse(NULL, 18, vin));

    TEST_ASSERT_EQUAL_STRING("untouched", vin);
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_id_parse_pdu2_broadcast);
    RUN_TEST(test_id_parse_pdu1_destination);
    RUN_TEST(test_id_make_round_trip);
    RUN_TEST(test_classify_kinds);
    RUN_TEST(test_classify_diag_and_foreign);
    RUN_TEST(test_bus_two_groups_make_j1939);
    RUN_TEST(test_bus_known_group_needs_eight_bytes);
    RUN_TEST(test_bus_other_traffic);
    RUN_TEST(test_bus_few_frames_stay_unknown);
    RUN_TEST(test_vin_with_delimiter);
    RUN_TEST(test_vin_without_delimiter_and_padded);
    RUN_TEST(test_vin_rejects_what_is_not_a_vin);

    run_tp_tests();
    run_values_tests();
    run_cache_tests();
    run_active_tests();

    UNITY_END();
}
