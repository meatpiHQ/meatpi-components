/**
 * @file test_voltage.c
 * @brief READ_VBATT: the production helper (j2534_server_voltage.c) against
 *        a fake battery owner. Rounding, a valid zero, a closed device, no
 *        sample, readings that are not a voltage, and the wire protocol's
 *        byte-exact example. See ../README.md. test_main.c runs the cases
 *        through run_voltage_tests() (it sits at the 700-line cap).
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "unity.h"
#include "battery_monitor.h"
#include "j2534_proto.h"
#include "j2534_server_private.h"

static float s_voltage;
static esp_err_t s_error;
static unsigned s_reads;

esp_err_t battery_monitor_voltage(float *out)
{
    s_reads++;
    if (s_error == ESP_OK)
    {
        *out = s_voltage;
    }
    return s_error;
}

static void test_voltage_millivolts(void)
{
    uint32_t result = 0;
    s_error = ESP_OK;
    s_voltage = 13.89f;
    TEST_ASSERT_EQUAL(J2534_STATUS_NOERROR, j2534_srv_read_vbatt(true, &result));
    TEST_ASSERT_EQUAL_UINT32(13890, result);
    s_voltage = 12.3456f;
    TEST_ASSERT_EQUAL(J2534_STATUS_NOERROR, j2534_srv_read_vbatt(true, &result));
    TEST_ASSERT_EQUAL_UINT32(12346, result);
    s_voltage = 0.0f;
    TEST_ASSERT_EQUAL(J2534_STATUS_NOERROR, j2534_srv_read_vbatt(true, &result));
    TEST_ASSERT_EQUAL_UINT32(0, result);
}

static void test_voltage_requires_open_device(void)
{
    uint32_t result = 0xDEADBEEF;
    s_reads = 0;
    s_error = ESP_OK;
    TEST_ASSERT_EQUAL(J2534_ERR_DEVICE_NOT_CONNECTED,
                      j2534_srv_read_vbatt(false, &result));
    TEST_ASSERT_EQUAL_UINT32(0, s_reads);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, result);
}

static void test_voltage_unavailable_and_invalid(void)
{
    uint32_t result = 0xDEADBEEF;
    s_error = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(J2534_ERR_FAILED, j2534_srv_read_vbatt(true, &result));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, result);
    s_error = ESP_OK;
    const float invalid[] = { NAN, INFINITY, -1.0f, FLT_MAX };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        s_voltage = invalid[i];
        TEST_ASSERT_EQUAL(J2534_ERR_FAILED, j2534_srv_read_vbatt(true, &result));
        TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, result);
    }
}

/* The byte-exact example of J2534_WIRE_PROTOCOL.md section 3 (the standard,
 * section 6c: a protocol document's examples are host-test vectors): the
 * request as a tester sends it, and the reply the device's own helper and
 * ACK encoder make of a 13.890 V reading. */
static void test_voltage_wire_vector(void)
{
    static const uint8_t REQUEST[] =
    {
        0x35, 0x4A, 0x01, 0x15, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
    };
    static const uint8_t REPLY[] =
    {
        0x35, 0x4A, 0x01, 0x80, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x42, 0x36, 0x00, 0x00,
    };
    static const uint8_t CLOSED[] =
    {
        0x35, 0x4A, 0x01, 0x80, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
    };
    j2534_hdr_t hdr;
    uint8_t buf[24];
    uint32_t millivolts = 0;

    TEST_ASSERT_TRUE(j2534_hdr_decode(REQUEST, sizeof(REQUEST), &hdr));
    TEST_ASSERT_EQUAL(J2534_MT_IOCTL, hdr.type);
    TEST_ASSERT_EQUAL(3, hdr.seq);
    TEST_ASSERT_EQUAL(0, hdr.channel);
    TEST_ASSERT_EQUAL(4, hdr.length);
    TEST_ASSERT_EQUAL(J2534_IOCTL_READ_VBATT,
                      REQUEST[12] | (REQUEST[13] << 8) | (REQUEST[14] << 16) |
                      ((uint32_t)REQUEST[15] << 24));

    s_error = ESP_OK;
    s_voltage = 13.89f;
    uint32_t status = j2534_srv_read_vbatt(true, &millivolts);
    TEST_ASSERT_EQUAL(J2534_STATUS_NOERROR, status);
    TEST_ASSERT_EQUAL(sizeof(REPLY), j2534_ack_encode(buf, sizeof(buf), hdr.seq,
                                                      hdr.channel, status, &millivolts));
    TEST_ASSERT_EQUAL_MEMORY(REPLY, buf, sizeof(REPLY));

    /* before OPEN and after CLOSE: the status alone, four payload bytes */
    status = j2534_srv_read_vbatt(false, &millivolts);
    TEST_ASSERT_EQUAL(sizeof(CLOSED), j2534_ack_encode(buf, sizeof(buf), hdr.seq,
                                                       hdr.channel, status, NULL));
    TEST_ASSERT_EQUAL_MEMORY(CLOSED, buf, sizeof(CLOSED));
}

void run_voltage_tests(void)
{
    RUN_TEST(test_voltage_millivolts);
    RUN_TEST(test_voltage_requires_open_device);
    RUN_TEST(test_voltage_unavailable_and_invalid);
    RUN_TEST(test_voltage_wire_vector);
}
