/**
 * @file test_voltage.c
 * @brief READ_VBATT: the production helper (j2534_server_voltage.c) against
 *        a fake battery owner. Rounding, a valid zero, a closed device, no
 *        sample, and readings that are not a voltage. See ../README.md.
 */
#include <float.h>
#include <math.h>

#include "unity.h"
#include "battery_monitor.h"
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

void test_voltage_millivolts(void)
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

void test_voltage_requires_open_device(void)
{
    uint32_t result = 0xDEADBEEF;
    s_reads = 0;
    s_error = ESP_OK;
    TEST_ASSERT_EQUAL(J2534_ERR_DEVICE_NOT_CONNECTED,
                      j2534_srv_read_vbatt(false, &result));
    TEST_ASSERT_EQUAL_UINT32(0, s_reads);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, result);
}

void test_voltage_unavailable_and_invalid(void)
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
