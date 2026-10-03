/**
 * @file test_values.c
 * @brief Host suite for j1939_dm_core.c (DM1 / DM2) and j1939_spn_core.c
 *        (the value table). Payloads are what the bench truck sends with
 *        its default values (tools/testbench/lib/j1939_ref.py, itself
 *        cross-checked against a J1939 DBC). Run from test_main.c.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "j1939_dm_core.h"
#include "j1939_spn_core.h"

/* ---- DM1 / DM2 ----------------------------------------------------------------- */

void test_dm_one_code_in_a_frame(void)
{
    /* SPN 110 FMI 0, seen 5 times, MIL on */
    static const uint8_t P[8] = { 0x40, 0xFF, 0x6E, 0x00, 0x00, 0x05, 0xFF, 0xFF };
    j1939_lamps_t lamps;
    j1939_dtc_t d[4];
    char text[J1939_DTC_TEXT_LEN];

    TEST_ASSERT_EQUAL_UINT32(1, j1939_dm_parse(P, sizeof(P), &lamps, d, 4));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_ON, lamps.mil);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_OFF, lamps.rsl);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_OFF, lamps.awl);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_OFF, lamps.pl);
    TEST_ASSERT_EQUAL_HEX8(0xFF, lamps.flash);
    TEST_ASSERT_EQUAL_UINT32(110, d[0].spn);
    TEST_ASSERT_EQUAL_UINT8(0, d[0].fmi);
    TEST_ASSERT_EQUAL_UINT8(5, d[0].oc);
    TEST_ASSERT_FALSE(d[0].cm);

    j1939_dtc_text(d[0].spn, d[0].fmi, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("SPN110-0", text);
}

void test_dm_no_code(void)
{
    static const uint8_t NONE[8] = { 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };
    static const uint8_t ONES[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    j1939_lamps_t lamps;
    j1939_dtc_t d[2];

    TEST_ASSERT_EQUAL_UINT32(0, j1939_dm_parse(NONE, sizeof(NONE), &lamps, d, 2));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_OFF, lamps.mil);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_OFF, lamps.awl);

    /* a controller without lamps and without codes */
    TEST_ASSERT_EQUAL_UINT32(0, j1939_dm_parse(ONES, sizeof(ONES), &lamps, d, 2));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_NA, lamps.mil);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_NA, lamps.pl);
}

void test_dm_three_codes_reassembled(void)
{
    /* MIL and amber on; SPN 110-0 x5, SPN 3226-4 x1, SPN 520192-31 x126 */
    static const uint8_t P[14] =
    {
        0x44, 0xFF, 0x6E, 0x00, 0x00, 0x05, 0x9A,
        0x0C, 0x04, 0x01, 0x00, 0xF0, 0xFF, 0x7E,
    };
    j1939_lamps_t lamps;
    j1939_dtc_t d[4];
    char text[J1939_DTC_TEXT_LEN];

    TEST_ASSERT_EQUAL_UINT32(3, j1939_dm_parse(P, sizeof(P), &lamps, d, 4));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_ON, lamps.mil);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_OFF, lamps.rsl);
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_ON, lamps.awl);
    TEST_ASSERT_EQUAL_UINT32(110, d[0].spn);
    TEST_ASSERT_EQUAL_UINT32(3226, d[1].spn);
    TEST_ASSERT_EQUAL_UINT8(4, d[1].fmi);
    TEST_ASSERT_EQUAL_UINT8(1, d[1].oc);
    /* the three top SPN bits live in the FMI byte */
    TEST_ASSERT_EQUAL_UINT32(520192, d[2].spn);
    TEST_ASSERT_EQUAL_UINT8(31, d[2].fmi);
    TEST_ASSERT_EQUAL_UINT8(126, d[2].oc);

    j1939_dtc_text(d[2].spn, d[2].fmi, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("SPN520192-31", text);

    /* less room than codes: the count is still the message's */
    memset(d, 0, sizeof(d));
    TEST_ASSERT_EQUAL_UINT32(3, j1939_dm_parse(P, sizeof(P), NULL, d, 2));
    TEST_ASSERT_EQUAL_UINT32(3226, d[1].spn);
    TEST_ASSERT_EQUAL_UINT32(0, d[2].spn);
    TEST_ASSERT_EQUAL_UINT32(3, j1939_dm_parse(P, sizeof(P), NULL, NULL, 0));
}

void test_dm_conversion_method_bit_and_short_payloads(void)
{
    static const uint8_t CM[8] = { 0x04, 0xFF, 0x6E, 0x00, 0x00, 0x85, 0xFF, 0xFF };
    j1939_lamps_t lamps;
    j1939_dtc_t d[2];
    char tiny[4];

    TEST_ASSERT_EQUAL_UINT32(1, j1939_dm_parse(CM, sizeof(CM), &lamps, d, 2));
    TEST_ASSERT_TRUE(d[0].cm);
    TEST_ASSERT_EQUAL_UINT8(5, d[0].oc);
    TEST_ASSERT_EQUAL_UINT32(110, d[0].spn);

    /* nothing, one byte: no lamps; five bytes: lamps, no whole record */
    TEST_ASSERT_EQUAL_UINT32(0, j1939_dm_parse(CM, 0, &lamps, d, 2));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_NA, lamps.mil);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_dm_parse(CM, 1, &lamps, d, 2));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_NA, lamps.awl);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_dm_parse(CM, 5, &lamps, d, 2));
    TEST_ASSERT_EQUAL_UINT8(J1939_LAMP_ON, lamps.awl);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_dm_parse(NULL, 8, &lamps, d, 2));

    j1939_dtc_text(110, 0, tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_STRING("SPN", tiny);
}

/* ---- the value table ------------------------------------------------------------ */

typedef struct
{
    const char *name;
    uint8_t     data[8];
    double      want;
    double      tol;
} vector_t;

/* the truck's default values: what j1939_ref.expected() lists, and the gear */
static const vector_t VECTORS[] =
{
    { "EngineSpeed",               { 0xF0, 0xAA, 0xA5, 0xE0, 0x2E, 0xFF, 0xFF, 0xFF }, 1500.0, 0.0 },
    { "ActualEnginePercentTorque", { 0xF0, 0xAA, 0xA5, 0xE0, 0x2E, 0xFF, 0xFF, 0xFF }, 40.0, 0.0 },
    { "AccelPedalPosition1",       { 0xFF, 0x50, 0x37, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 32.0, 1e-9 },
    { "EnginePercentLoad",         { 0xFF, 0x50, 0x37, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 55.0, 0.0 },
    { "TransCurrentGear",          { 0x87, 0xFF, 0xFF, 0x87, 0xFF, 0xFF, 0xFF, 0xFF }, 10.0, 0.0 },
    { "WheelBasedVehicleSpeed",    { 0xFF, 0x80, 0x52, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 82.5, 0.0 },
    { "EngineCoolantTemperature",  { 0x7E, 0x4E, 0x50, 0x2E, 0xFF, 0xFF, 0xFF, 0xFF }, 86.0, 0.0 },
    { "EngineOilTemperature",      { 0x7E, 0x4E, 0x50, 0x2E, 0xFF, 0xFF, 0xFF, 0xFF }, 97.5, 0.0 },
    { "EngineOilPressure",         { 0x64, 0xFF, 0xC8, 0x5A, 0xFF, 0xFF, 0xFF, 0xE1 }, 360.0, 0.0 },
    { "IntakeManifoldPressure",    { 0xFF, 0x5A, 0x55, 0xFF, 0xFF, 0xA0, 0x56, 0xFF }, 180.0, 0.0 },
    { "IntakeManifoldTemperature", { 0xFF, 0x5A, 0x55, 0xFF, 0xFF, 0xA0, 0x56, 0xFF }, 45.0, 0.0 },
    { "BarometricPressure",        { 0xC8, 0xFF, 0xFF, 0xD0, 0x24, 0x41, 0xFF, 0xFF }, 100.0, 0.0 },
    { "AmbientAirTemperature",     { 0xC8, 0xFF, 0xFF, 0xD0, 0x24, 0x41, 0xFF, 0xFF }, 21.5, 0.0 },
    { "BatteryPotential",          { 0xFF, 0xFF, 0x30, 0x02, 0x28, 0x02, 0x24, 0x02 }, 27.6, 1e-9 },
    { "FuelRate",                  { 0xD6, 0x01, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xFF }, 23.5, 1e-9 },
    { "FuelLevel1",                { 0xFF, 0x9B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 62.0, 1e-9 },
    { "DEFTankLevel",              { 0xAF, 0x46, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 70.0, 1e-9 },
    { "TotalVehicleDistanceHR",    { 0xA4, 0xED, 0x1E, 0x04, 0xFF, 0xFF, 0xFF, 0xFF }, 345678.9, 1e-6 },
    { "TotalVehicleDistance",      { 0xE4, 0x0C, 0x00, 0x00, 0x77, 0x32, 0x2A, 0x00 }, 345678.875, 0.0 },
    { "EngineTotalHours",          { 0xA5, 0x7A, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF }, 8123.45, 1e-6 },
    { "EngineTotalFuelUsed",       { 0x2C, 0x01, 0x00, 0x00, 0x81, 0xC4, 0x03, 0x00 }, 123456.5, 0.0 },
};

#define N_VECTORS (sizeof(VECTORS) / sizeof(VECTORS[0]))

void test_spn_decodes_the_trucks_values(void)
{
    for (size_t i = 0; i < N_VECTORS; i++)
    {
        const vector_t *v = &VECTORS[i];
        const j1939_spn_t *s = j1939_spn_find(v->name);
        double got = -1e30;

        TEST_ASSERT_NOT_NULL_MESSAGE(s, v->name);
        TEST_ASSERT_EQUAL_MESSAGE(J1939_RAW_VALID,
                                  j1939_spn_decode(s, v->data, 8, &got),
                                  v->name);
        TEST_ASSERT_TRUE_MESSAGE(fabs(got - v->want) <= v->tol, v->name);
        /* and it is inside the range a consumer clamps to */
        TEST_ASSERT_TRUE_MESSAGE(got >= s->min && got <= s->max, v->name);
    }
}

void test_spn_table_is_what_the_vectors_cover(void)
{
    size_t n = 0;
    const j1939_spn_t *t = j1939_spn_table(&n);

    TEST_ASSERT_EQUAL_UINT32(N_VECTORS, n);

    for (size_t i = 0; i < n; i++)
    {
        bool covered = false;

        for (size_t k = 0; k < N_VECTORS; k++)
        {
            covered = covered || strcmp(VECTORS[k].name, t[i].name) == 0;
        }

        TEST_ASSERT_TRUE_MESSAGE(covered, t[i].name);
        TEST_ASSERT_TRUE_MESSAGE(t[i].min < t[i].max, t[i].name);
        TEST_ASSERT_NOT_NULL(t[i].unit);
        TEST_ASSERT_NOT_NULL(t[i].dev_class);

        /* names are keys: no two alike */
        for (size_t k = i + 1; k < n; k++)
        {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(t[i].name, t[k].name) != 0,
                                     t[i].name);
        }
    }

    TEST_ASSERT_NULL(j1939_spn_find("EngineRPM"));
    TEST_ASSERT_NULL(j1939_spn_find(NULL));
}

void test_spn_names_differ_from_the_obd_tables(void)
{
    /* the OBD names a J1939 label comes close to (autopid's
       obd2_standard_pids.h): a vehicle may carry both sets */
    static const char *const OBD[] =
    {
        "EngineRPM", "VehicleSpeed", "EngineCoolantTemp", "EngineOilTemp",
        "AmbientAirTemp", "EngineFuelRate", "FuelTankLevel", "Odometer",
        "AbsBaroPres", "IntakeAirTemperature", "ControlModuleVolt",
        "CalcEngineLoad", "RelAccelPedalPos", "TransmissionActualGear",
        "IntakeManifoldAbsolutePressure", "EngineRunTime",
    };

    for (size_t i = 0; i < sizeof(OBD) / sizeof(OBD[0]); i++)
    {
        TEST_ASSERT_NULL_MESSAGE(j1939_spn_find(OBD[i]), OBD[i]);
    }
}

void test_spn_not_available_error_and_the_range_edge(void)
{
    const j1939_spn_t *rpm = j1939_spn_find("EngineSpeed");
    const j1939_spn_t *torque = j1939_spn_find("ActualEnginePercentTorque");
    const j1939_spn_t *gear = j1939_spn_find("TransCurrentGear");
    const j1939_spn_t *dist = j1939_spn_find("TotalVehicleDistance");
    uint8_t p[8];
    double v = 123.0;

    /* the sender has no engine speed */
    memset(p, 0xFF, sizeof(p));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_spn_decode(rpm, p, 8, &v));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_spn_decode(torque, p, 8, &v));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_spn_decode(dist, p, 8, &v));

    /* it cannot measure it right now */
    memset(p, 0xFE, sizeof(p));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_spn_decode(rpm, p, 8, &v));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_spn_decode(torque, p, 8, &v));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_spn_decode(dist, p, 8, &v));

    /* only the top byte decides for a two-byte value */
    memset(p, 0x00, sizeof(p));
    p[4] = 0xFB;
    TEST_ASSERT_EQUAL(J1939_RAW_SPECIFIC, j1939_spn_decode(rpm, p, 8, &v));
    p[4] = 0xFC;
    TEST_ASSERT_EQUAL(J1939_RAW_RESERVED, j1939_spn_decode(rpm, p, 8, &v));
    p[4] = 0xFD;
    TEST_ASSERT_EQUAL(J1939_RAW_RESERVED, j1939_spn_decode(rpm, p, 8, &v));
    TEST_ASSERT_EQUAL_DOUBLE(123.0, v); /* never written without a value */

    /* the largest valid raw value is the table's max */
    p[3] = 0xFF;
    p[4] = 0xFA;
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_spn_decode(rpm, p, 8, &v));
    TEST_ASSERT_EQUAL_DOUBLE(8031.875, v);
    TEST_ASSERT_EQUAL_DOUBLE(rpm->max, v);

    /* "park" is a parameter specific indicator of the gear, not a number */
    memset(p, 0x00, sizeof(p));
    p[3] = 0xFB;
    TEST_ASSERT_EQUAL(J1939_RAW_SPECIFIC, j1939_spn_decode(gear, p, 8, &v));
    p[3] = 0x7C; /* reverse 1 */
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_spn_decode(gear, p, 8, &v));
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, v);

    /* a message too short to hold the value; a value is optional */
    TEST_ASSERT_EQUAL(J1939_RAW_SHORT, j1939_spn_decode(rpm, p, 4, &v));
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_spn_decode(rpm, p, 5, NULL));
    TEST_ASSERT_EQUAL(J1939_RAW_SHORT, j1939_spn_decode(NULL, p, 8, &v));
    TEST_ASSERT_EQUAL(J1939_RAW_SHORT, j1939_spn_decode(rpm, NULL, 8, &v));
}

void test_raw_class_by_width(void)
{
    /* one byte */
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_raw_class(0xFA, 8));
    TEST_ASSERT_EQUAL(J1939_RAW_SPECIFIC, j1939_raw_class(0xFB, 8));
    TEST_ASSERT_EQUAL(J1939_RAW_RESERVED, j1939_raw_class(0xFC, 8));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_raw_class(0xFE, 8));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_raw_class(0xFF, 8));

    /* four bytes: FAFFFFFF is the last valid one */
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_raw_class(0xFAFFFFFFu, 32));
    TEST_ASSERT_EQUAL(J1939_RAW_SPECIFIC, j1939_raw_class(0xFB000000u, 32));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_raw_class(0xFE123456u, 32));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_raw_class(0xFFFFFFFFu, 32));

    /* discrete fields: all ones = not available, one less = error */
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_raw_class(1, 2));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_raw_class(2, 2));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_raw_class(3, 2));
    TEST_ASSERT_EQUAL(J1939_RAW_ERROR, j1939_raw_class(0xE, 4));
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_raw_class(0xF, 4));
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_raw_class(1, 1));

    TEST_ASSERT_EQUAL_STRING("valid", j1939_raw_name(J1939_RAW_VALID));
    TEST_ASSERT_EQUAL_STRING("na", j1939_raw_name(J1939_RAW_NOT_AVAILABLE));
    TEST_ASSERT_EQUAL_STRING("error", j1939_raw_name(J1939_RAW_ERROR));
    TEST_ASSERT_EQUAL_STRING("short", j1939_raw_name(J1939_RAW_SHORT));
}

void test_spn_bit_field_inside_a_byte(void)
{
    /* not in the table: a two-bit switch at bits 2..3 of byte 0, and a
       12-bit value that starts in the middle of a byte */
    static const j1939_spn_t SW = { 0, 0, "sw", "", "", 0, 2, 2, 1.0, 0.0, 0.0, 3.0 };
    static const j1939_spn_t W12 = { 0, 0, "w", "", "", 1, 4, 12, 0.5, -10.0, 0.0, 0.0 };
    uint8_t p[8] = { 0x04, 0x30, 0x12, 0, 0, 0, 0, 0 };
    double v = 0;

    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_spn_decode(&SW, p, 8, &v));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, v);
    p[0] = 0x0C;
    TEST_ASSERT_EQUAL(J1939_RAW_NOT_AVAILABLE, j1939_spn_decode(&SW, p, 8, &v));

    /* bits 4..7 of byte 1 (3) are the low nibble, byte 2 (0x12) the rest:
       raw = 0x123 = 291 -> 291 * 0.5 - 10 */
    TEST_ASSERT_EQUAL(J1939_RAW_VALID, j1939_spn_decode(&W12, p, 8, &v));
    TEST_ASSERT_EQUAL_DOUBLE(135.5, v);
    TEST_ASSERT_EQUAL(J1939_RAW_SHORT, j1939_spn_decode(&W12, p, 2, &v));
}

void run_values_tests(void)
{
    RUN_TEST(test_dm_one_code_in_a_frame);
    RUN_TEST(test_dm_no_code);
    RUN_TEST(test_dm_three_codes_reassembled);
    RUN_TEST(test_dm_conversion_method_bit_and_short_payloads);
    RUN_TEST(test_spn_decodes_the_trucks_values);
    RUN_TEST(test_spn_table_is_what_the_vectors_cover);
    RUN_TEST(test_spn_names_differ_from_the_obd_tables);
    RUN_TEST(test_spn_not_available_error_and_the_range_edge);
    RUN_TEST(test_raw_class_by_width);
    RUN_TEST(test_spn_bit_field_inside_a_byte);
}
