/**
 * @file test_main.c
 * @brief Host suite for usb_acm_gps_parse — the ESPNetLink `gps -p -j`
 *        JSON → fix mapping that GPS values ride into autopid (HA push /
 *        data_logger / dashboard / event rules). Fixtures are real
 *        captures from a BG95-M5 dongle.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "usb_acm_gps.h"

/* live fix (indoor bench would be valid:false; this is a synthesised
 * valid fix with realistic magnitudes), wrapped in the console echo +
 * esp> prompt exactly as /api/usb/acm/cmd returns it */
static const char *FIX_WRAPPED =
    "gps -p -j\r\n\r\r\n{\"valid\":true,\"lat\":-37.9053500,"
    "\"lon\":145.1450470,\"satellites\":7,\"sats_in_view\":11,"
    "\"fix_quality\":1,\"fix_type\":3,\"altitude_m\":88.8,\"hdop\":1.2,"
    "\"pdop\":1.9,\"vdop\":1.4,\"speed_knots\":0.02,\"speed_kmph\":3.6,"
    "\"course_deg\":270.5,\"age_ms\":480,\"agnss_enabled\":true,"
    "\"cached_lat\":-37.905350,\"cached_lon\":145.145047,"
    "\"cached_alt\":88.8}\r\r\nOK\r\r\nesp> \r\nOK\r\nwican> ";

/* no live fix — the indoor default: valid:false but a cached position
 * present. Must NOT be reported (a cached position is not "current"). */
static const char *NO_FIX =
    "{\"valid\":false,\"lat\":0.0000000,\"lon\":0.0000000,"
    "\"satellites\":0,\"sats_in_view\":0,\"hdop\":100.0,"
    "\"altitude_m\":0.0,\"speed_kmph\":0.0,\"course_deg\":0.0,"
    "\"cached_lat\":-37.905350,\"cached_lon\":145.145047}";

void test_valid_fix(void)
{
    usb_acm_gps_t g;

    TEST_ASSERT_TRUE(usb_acm_gps_parse(FIX_WRAPPED, &g));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -37.90535, g.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 145.145047, g.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 88.8, g.altitude_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 3.6, g.speed_kmph);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 270.5, g.heading_deg);
    TEST_ASSERT_EQUAL_INT(7, g.satellites);
    TEST_ASSERT_EQUAL_INT(6, g.accuracy_m); /* round(1.2 * 5) */
}

void test_no_fix_is_rejected(void)
{
    usb_acm_gps_t g;

    /* valid:false → not a fix, and the cached position is NOT adopted */
    TEST_ASSERT_FALSE(usb_acm_gps_parse(NO_FIX, &g));
    TEST_ASSERT_FALSE(g.valid);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g.latitude);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g.longitude);
}

void test_lat_not_confused_with_cached_lat(void)
{
    /* the leading quote in "lat": must not match inside "cached_lat": —
     * a cached_lat FIRST in the object would otherwise be read as lat */
    static const char *reordered =
        "{\"cached_lat\":11.111111,\"cached_lon\":22.222222,"
        "\"valid\":true,\"lat\":-1.234567,\"lon\":9.876543,"
        "\"hdop\":2.0,\"altitude_m\":5,\"speed_kmph\":0,\"course_deg\":0}";
    usb_acm_gps_t g;

    TEST_ASSERT_TRUE(usb_acm_gps_parse(reordered, &g));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -1.234567, g.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 9.876543, g.longitude);
    TEST_ASSERT_EQUAL_INT(10, g.accuracy_m); /* round(2.0 * 5) */
}

void test_valid_without_coords_rejected(void)
{
    usb_acm_gps_t g;

    TEST_ASSERT_FALSE(usb_acm_gps_parse("{\"valid\":true}", &g));
}

void test_garbage_and_null(void)
{
    usb_acm_gps_t g;

    TEST_ASSERT_FALSE(usb_acm_gps_parse("not json", &g));
    TEST_ASSERT_FALSE(usb_acm_gps_parse("", &g));
    TEST_ASSERT_FALSE(usb_acm_gps_parse(NULL, &g));
    TEST_ASSERT_FALSE(usb_acm_gps_parse("{}", &g));
}

void test_negative_and_zero_heading(void)
{
    /* a due-north 0.0 course + southern-hemisphere negative lat must
     * both round-trip (no sign/zero mishandling) */
    static const char *north =
        "{\"valid\":true,\"lat\":-45.5,\"lon\":-120.25,\"hdop\":0.8,"
        "\"altitude_m\":-3.2,\"speed_kmph\":88.0,\"course_deg\":0.0,"
        "\"satellites\":12}";
    usb_acm_gps_t g;

    TEST_ASSERT_TRUE(usb_acm_gps_parse(north, &g));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -45.5, g.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -120.25, g.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -3.2, g.altitude_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.0, g.heading_deg);
    TEST_ASSERT_EQUAL_INT(12, g.satellites);
    TEST_ASSERT_EQUAL_INT(4, g.accuracy_m); /* round(0.8 * 5) */
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_valid_fix);
    RUN_TEST(test_no_fix_is_rejected);
    RUN_TEST(test_lat_not_confused_with_cached_lat);
    RUN_TEST(test_valid_without_coords_rejected);
    RUN_TEST(test_garbage_and_null);
    RUN_TEST(test_negative_and_zero_heading);

    UNITY_END();
}
