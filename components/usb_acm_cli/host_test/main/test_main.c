/**
 * @file test_main.c
 * @brief Host suite for usb_acm_gps_parse: the ESPNetLink `gps -p -j`
 *        JSON → fix mapping that GPS values ride into autopid (HA push /
 *        data_logger / dashboard / event rules). Fixtures are real
 *        captures from a BG95-M5 dongle.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "usb_acm_gps.h"
#include "usb_acm_nmea.h"

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

/* no live fix: the indoor default: valid:false but a cached position
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
    /* the leading quote in "lat": must not match inside "cached_lat":
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

/* ---- NMEA 0183 from a plain USB receiver (2026-10-07) ---------------------- */

/* a u-blox 7 on the bench, 1 Hz: the GGA then the RMC of one second */
static const char *NMEA_GGA =
    "$GPGGA,081836.00,3754.3210,S,14508.7028,E,1,09,1.20,88.8,M,-3.2,M,,*6B";
static const char *NMEA_RMC =
    "$GPRMC,081836.00,A,3754.3210,S,14508.7028,E,0.65,270.50,071026,,,A*46";
static const char *NMEA_RMC_V =
    "$GPRMC,081837.00,V,,,,,,,071026,,,N*7A";
static const char *NMEA_RMC_GN =
    "$GNRMC,120000.00,A,4807.0380,N,01131.0000,E,10.00,90.00,071026,,,A*4B";
static const char *NMEA_GSV =
    "$GPGSV,3,1,11,01,45,120,40,02,30,210,35,03,60,045,42,04,15,300,28*73";
static const char *NMEA_VTG = "$GPVTG,270.50,T,,M,0.65,N,1.20,K,A*3D";
static const char *NMEA_GGA_NOFIX =
    "$GPGGA,081900.00,3754.3210,S,14508.7028,E,0,00,99.99,,M,,M,,*40";

void test_nmea_gga_then_rmc_is_a_fix(void)
{
    usb_acm_nmea_t st;
    usb_acm_gps_t g;

    usb_acm_nmea_init(&st);
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_line(&st, NMEA_GGA, &g));
    TEST_ASSERT_TRUE(st.have_gga);
    TEST_ASSERT_EQUAL_INT(1, usb_acm_nmea_feed_line(&st, NMEA_RMC, &g));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -37.905350, g.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 145.145047, g.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 1.2038, g.speed_kmph);   /* 0.65 kn */
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 270.5, g.heading_deg);
    TEST_ASSERT_EQUAL_INT(9, g.satellites);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 88.8, g.altitude_m);
    TEST_ASSERT_EQUAL_INT(6, g.accuracy_m);   /* round(1.2 * 5) */
    TEST_ASSERT_EQUAL_UINT32(2, st.sentences);
    TEST_ASSERT_EQUAL_UINT32(0, st.bad_checksum);
}

void test_nmea_rmc_alone_and_other_talker(void)
{
    usb_acm_nmea_t st;
    usb_acm_gps_t g;

    usb_acm_nmea_init(&st);
    /* no GGA yet: a fix all the same, the GGA-only fields at zero */
    TEST_ASSERT_EQUAL_INT(1, usb_acm_nmea_feed_line(&st, NMEA_RMC_GN, &g));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.117300, g.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 11.516667, g.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 18.52, g.speed_kmph);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 90.0, g.heading_deg);
    TEST_ASSERT_EQUAL_INT(0, g.satellites);
    TEST_ASSERT_EQUAL_INT(0, g.accuracy_m);
}

void test_nmea_lost_fix_and_other_sentences(void)
{
    usb_acm_nmea_t st;
    usb_acm_gps_t g;

    usb_acm_nmea_init(&st);
    TEST_ASSERT_EQUAL_INT(-1, usb_acm_nmea_feed_line(&st, NMEA_RMC_V, &g));
    /* a GGA without a fix is still remembered (sats 0), not a verdict */
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_line(&st, NMEA_GGA_NOFIX, &g));
    TEST_ASSERT_EQUAL_INT(0, st.satellites);
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_line(&st, NMEA_GSV, &g));
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_line(&st, NMEA_VTG, &g));
    TEST_ASSERT_EQUAL_UINT32(4, st.sentences);
    TEST_ASSERT_TRUE(usb_acm_nmea_is_sentence(NMEA_GSV));
    /* the ESPNetLink's console is not NMEA */
    TEST_ASSERT_FALSE(usb_acm_nmea_is_sentence("esp> "));
    TEST_ASSERT_FALSE(usb_acm_nmea_is_sentence("{\"valid\":true}"));
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_line(&st, "esp> ", &g));
    TEST_ASSERT_EQUAL_UINT32(4, st.sentences);
}

void test_nmea_bad_checksum_is_dropped(void)
{
    usb_acm_nmea_t st;
    usb_acm_gps_t g;

    usb_acm_nmea_init(&st);
    /* one byte of the body flipped: the checksum no longer matches */
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_line(&st,
        "$GPRMC,081836.00,A,3754.3211,S,14508.7028,E,0.65,270.50,071026,,,A*46", &g));
    TEST_ASSERT_EQUAL_UINT32(1, st.bad_checksum);
    TEST_ASSERT_EQUAL_UINT32(0, st.sentences);
    TEST_ASSERT_FALSE(usb_acm_nmea_is_sentence("$GPRMC,1,A*ZZ"));
    TEST_ASSERT_FALSE(usb_acm_nmea_is_sentence("$GPRMC"));
    TEST_ASSERT_FALSE(usb_acm_nmea_is_sentence(NULL));
}

void test_nmea_bytes_any_chunking(void)
{
    usb_acm_nmea_t st;
    usb_acm_gps_t g;
    char stream[400];

    usb_acm_nmea_init(&st);
    snprintf(stream, sizeof(stream), "%s\r\n%s\r\n%s\r\n", NMEA_GSV, NMEA_GGA, NMEA_RMC);

    /* 7 bytes at a time: sentences span calls; the chunk holding the RMC's
       end is the one that yields the fix */
    size_t len = strlen(stream);
    int fixes = 0;

    memset(&g, 0, sizeof(g));
    for (size_t off = 0; off < len; off += 7)
    {
        size_t n = len - off < 7 ? len - off : 7;
        int r = usb_acm_nmea_feed_bytes(&st, (const uint8_t *)stream + off, n, &g);

        if (r > 0)
        {
            fixes++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, fixes);
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_EQUAL_INT(9, g.satellites);
    TEST_ASSERT_EQUAL_UINT32(3, st.sentences);

    /* a lost fix at the end of a chunk */
    snprintf(stream, sizeof(stream), "%s\r\n", NMEA_RMC_V);
    TEST_ASSERT_EQUAL_INT(-1, usb_acm_nmea_feed_bytes(&st, (const uint8_t *)stream, strlen(stream), &g));

    /* an over-long line is dropped whole, the next sentence is fine */
    memset(stream, 'X', 150);
    stream[0] = '$';
    memcpy(stream + 150, "\r\n", 3);
    TEST_ASSERT_EQUAL_INT(0, usb_acm_nmea_feed_bytes(&st, (const uint8_t *)stream, strlen(stream), &g));
    snprintf(stream, sizeof(stream), "%s\r\n", NMEA_RMC_GN);
    TEST_ASSERT_EQUAL_INT(1, usb_acm_nmea_feed_bytes(&st, (const uint8_t *)stream, strlen(stream), &g));
    TEST_ASSERT_EQUAL_UINT32(5, st.sentences);
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
    RUN_TEST(test_nmea_gga_then_rmc_is_a_fix);
    RUN_TEST(test_nmea_rmc_alone_and_other_talker);
    RUN_TEST(test_nmea_lost_fix_and_other_sentences);
    RUN_TEST(test_nmea_bad_checksum_is_dropped);
    RUN_TEST(test_nmea_bytes_any_chunking);

    UNITY_END();
}
