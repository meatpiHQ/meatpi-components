/**
 * @file test_resp_lines.c
 * @brief Host suite for the reply parser cases of TASK_j1939_wwh.md phase 3
 *        (autopid_resp.c + autopid_resp_lines.c): the chip's 29-bit id
 *        print with headers on (four byte tokens), what must NOT be taken
 *        for such an id, `7F xx 78` response-pending lines, and the
 *        two-byte identifier check of service 22. The reply texts are the
 *        OBD chip's own, captured on the bench 2026-10-03. Run from
 *        test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

void test_resp_29bit_headers_on_captured(void)
{
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n = 0;

    /* single frame */
    const uint8_t rpm[] = { 0x62, 0xF4, 0x0C, 0x13, 0x88 };

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("18 DA F1 00 05 62 F4 0C 13 88 \r",
                                         p, sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(rpm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, p, sizeof(rpm));

    /* a classic 29-bit car: 0100 with headers on */
    const uint8_t bm[] = { 0x41, 0x00, 0xBE, 0x3F, 0xA8, 0x13 };

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload(
                          "18 DA F1 10 06 41 00 BE 3F A8 13 \r", p,
                          sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(bm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bm, p, sizeof(bm));

    /* two responders, each kept apart, ascending by id */
    static ap_resp_ecu_t ecus[AP_RESP_ECUS_MAX];
    int k = ap_resp_to_payloads("18 DA F1 3D 07 62 F4 01 00 07 65 04 \r"
                                "18 DA F1 00 07 62 F4 01 81 07 65 04 \r",
                                ecus, AP_RESP_ECUS_MAX);

    TEST_ASSERT_EQUAL(2, k);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF100, ecus[0].header);
    TEST_ASSERT_EQUAL_HEX8(0x81, ecus[0].payload[3]);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF13D, ecus[1].header);
    TEST_ASSERT_EQUAL_HEX8(0x00, ecus[1].payload[3]);

    /* bench capture: 19 42 33 0C 1E, both ECUs ask for time first, the
       engine answers in three frames (16 bytes), the other in one */
    const uint8_t dtc0[] =
    {
        0x59, 0x42, 0x33, 0xFF, 0xFF, 0x04, 0x02, 0x04, 0x20, 0x00, 0x08,
        0x04, 0x24, 0x63, 0x1F, 0x04,
    };
    const uint8_t dtc1[] = { 0x59, 0x42, 0x33, 0xFF, 0xFF, 0x04 };

    k = ap_resp_to_payloads("18 DA F1 00 03 7F 19 78 \r"
                            "18 DA F1 3D 03 7F 19 78 \r"
                            "18 DA F1 00 10 10 59 42 33 FF FF 04 \r"
                            "18 DA F1 3D 06 59 42 33 FF FF 04 \r"
                            "18 DA F1 00 21 02 04 20 00 08 04 24 \r"
                            "18 DA F1 00 22 63 1F 04 00 00 00 00 \r",
                            ecus, AP_RESP_ECUS_MAX);
    TEST_ASSERT_EQUAL(2, k);
    TEST_ASSERT_EQUAL_size_t(sizeof(dtc0), ecus[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(dtc0, ecus[0].payload, sizeof(dtc0));
    TEST_ASSERT_EQUAL_size_t(sizeof(dtc1), ecus[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(dtc1, ecus[1].payload, sizeof(dtc1));
}

void test_resp_29bit_is_not_guessed(void)
{
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n = 0;

    /* too short to be id + PCI + data: plain bytes, as before */
    TEST_ASSERT_EQUAL(ESP_OK, ap_resp_to_payload("18 DA F1 58 01\r", p,
                                                 sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_HEX8(0x18, p[0]);

    /* the byte after the four is no PCI for this line (a flow control
       nibble; a single frame longer than what follows) */
    TEST_ASSERT_EQUAL(ESP_OK, ap_resp_to_payload("18 DA F1 58 30 00 00\r", p,
                                                 sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(7, n);
    TEST_ASSERT_EQUAL(ESP_OK, ap_resp_to_payload("18 DA F1 58 07 62 F4\r", p,
                                                 sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(7, n);

    /* another 29-bit id is not the legislated answer: left alone */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("18 DB 33 F1 02 01 00\r", p,
                                         sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(7, n);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("17 FC 00 76 03 62 F1 90\r", p,
                                         sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(8, n);

    /* headers-off answers start with the service echo and stay whole */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("62 F4 A6 00 12 D6 87 \r", p,
                                         sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(7, n);
    TEST_ASSERT_EQUAL_HEX8(0x62, p[0]);
}

void test_resp_pending_lines(void)
{
    uint8_t p[AP_PAYLOAD_MAX];
    size_t n = 0;

    /* headers off: 7F 22 78, then the answer on one line. Until
       2026-10-03 the two lines were one payload starting with 7F */
    const uint8_t rpm[] = { 0x62, 0xF4, 0x0C, 0x13, 0x88 };

    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("7F 22 78 \r62 F4 0C 13 88 \r", p,
                                         sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(rpm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, p, sizeof(rpm));

    /* twice pending, 11-bit headers on, padded frames */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("7E8 03 7F 22 78 00 00 00 00\r"
                                         "7E8 03 7F 22 78 00 00 00 00\r"
                                         "7E8 05 62 F4 0C 13 88 00 00\r",
                                         p, sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(rpm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, p, sizeof(rpm));

    /* the answer never came: no payload at all */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ap_resp_to_payload("7F 22 78 \r", p, sizeof(p), &n));

    /* any other negative response is the ECU's answer and stays */
    TEST_ASSERT_EQUAL(ESP_OK, ap_resp_to_payload("7F 22 31 \r", p,
                                                 sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x31, p[2]);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ap_resp_to_payload("18 DA F1 00 03 7F 22 31 \r", p,
                                         sizeof(p), &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x7F, p[0]);
}

void test_resp_matches_cmd_two_byte_identifier(void)
{
    const uint8_t f40c[] = { 0x62, 0xF4, 0x0C, 0x13, 0x88 };
    const uint8_t f40d[] = { 0x62, 0xF4, 0x0D, 0x40 };
    const uint8_t short_echo[] = { 0x62, 0xF4 };
    const uint8_t kwp[] = { 0x61, 0x01, 0x02, 0x03 };
    const uint8_t dtc[] = { 0x59, 0x42, 0x33, 0xFF };

    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22F40C", f40c, sizeof(f40c)));
    /* the late answer to the previous request is not this one's */
    TEST_ASSERT_FALSE(ap_payload_matches_cmd("22F40C", f40d, sizeof(f40d)));
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22F40D", f40d, sizeof(f40d)));
    /* the response-count hint digit is not a byte */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22F40C1", f40c, sizeof(f40c)));
    /* several identifiers in one request: the first one's echo decides */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22F40CF40D", f40c,
                                            sizeof(f40c)));
    /* a payload cut after two bytes cannot be told apart: as before */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22F40C", short_echo,
                                            sizeof(short_echo)));
    /* only service 22 carries a 2-byte identifier here */
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("210199", kwp, sizeof(kwp)));
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("19423399", dtc, sizeof(dtc)));
}

void run_resp_lines_tests(void)
{
    RUN_TEST(test_resp_29bit_headers_on_captured);
    RUN_TEST(test_resp_29bit_is_not_guessed);
    RUN_TEST(test_resp_pending_lines);
    RUN_TEST(test_resp_matches_cmd_two_byte_identifier);
}
