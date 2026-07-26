/**
 * @file test_main.c
 * @brief Host suite for can_core_filter — the pure CAN frame filter/mask
 *        match, id parse (11/29-bit), byte/byte-string parse, and hex-id
 *        formatting. This is the RX-dispatch logic can_manager and the
 *        ELM327 CAN path rely on (can_core.c:can_subscription_matches_
 *        frame → can_core_filter_match).
 */
#include <string.h>

#include "unity.h"

#include "can_core_filter.h"
#include "can_core_recovery.h"

/* ---- filter / mask match ------------------------------------------------ */

void test_filter_match_exact(void)
{
    /* full mask = exact id match */
    TEST_ASSERT_TRUE(can_core_filter_match(0x7E8, 0x7E8, 0x7FF));
    TEST_ASSERT_FALSE(can_core_filter_match(0x7E9, 0x7E8, 0x7FF));
}

void test_filter_match_pass_all(void)
{
    /* mask 0 = accept every id */
    TEST_ASSERT_TRUE(can_core_filter_match(0x000, 0x7E8, 0x000));
    TEST_ASSERT_TRUE(can_core_filter_match(0x1FFFFFFF, 0x7E8, 0x000));
}

void test_filter_match_partial_mask(void)
{
    /* mask 0x7F0 ignores the low nibble: 0x7E0..0x7EF all match 0x7E8 */
    TEST_ASSERT_TRUE(can_core_filter_match(0x7E0, 0x7E8, 0x7F0));
    TEST_ASSERT_TRUE(can_core_filter_match(0x7EF, 0x7E8, 0x7F0));
    TEST_ASSERT_FALSE(can_core_filter_match(0x7D8, 0x7E8, 0x7F0));
}

void test_filter_match_29bit(void)
{
    TEST_ASSERT_TRUE(can_core_filter_match(0x18DAF110, 0x18DAF110,
                                             0x1FFFFFFF));
    TEST_ASSERT_FALSE(can_core_filter_match(0x18DAF111, 0x18DAF110,
                                              0x1FFFFFFF));
}

/* ---- id parse ----------------------------------------------------------- */

void test_parse_id_11bit(void)
{
    uint32_t id = 0;
    bool ext = true;

    TEST_ASSERT_TRUE(can_core_parse_id("7E0", &id, &ext));
    TEST_ASSERT_EQUAL_HEX32(0x7E0, id);
    TEST_ASSERT_FALSE(ext);
}

void test_parse_id_29bit(void)
{
    uint32_t id = 0;
    bool ext = false;

    TEST_ASSERT_TRUE(can_core_parse_id("18DAF110", &id, &ext));
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, id);
    TEST_ASSERT_TRUE(ext);
}

void test_parse_id_masks_overflow(void)
{
    uint32_t id = 0;
    bool ext = false;

    /* 3-digit result is masked to 11 bits */
    TEST_ASSERT_TRUE(can_core_parse_id("FFF", &id, &ext));
    TEST_ASSERT_EQUAL_HEX32(0x7FF, id);
    TEST_ASSERT_FALSE(ext);
}

void test_parse_id_rejects_non_hex(void)
{
    uint32_t id = 0;
    bool ext = false;

    TEST_ASSERT_FALSE(can_core_parse_id("7EZ", &id, &ext));
    TEST_ASSERT_FALSE(can_core_parse_id("", &id, &ext));
    TEST_ASSERT_FALSE(can_core_parse_id("123456789", &id, &ext)); /* >8 */
}

/* ---- byte / byte-string parse ------------------------------------------- */

void test_parse_byte(void)
{
    uint8_t b = 0;

    TEST_ASSERT_TRUE(can_core_parse_byte("A", &b));
    TEST_ASSERT_EQUAL_HEX8(0x0A, b);
    TEST_ASSERT_TRUE(can_core_parse_byte("FF", &b));
    TEST_ASSERT_EQUAL_HEX8(0xFF, b);
    TEST_ASSERT_FALSE(can_core_parse_byte("", &b));
    TEST_ASSERT_FALSE(can_core_parse_byte("1FF", &b)); /* >2 digits */
    TEST_ASSERT_FALSE(can_core_parse_byte("GG", &b));
}

void test_parse_bytes(void)
{
    uint8_t buf[8];
    size_t n = 0;

    /* a UDS request "10 02" style pair (whatever separator the parser uses
     * for a contiguous hex string) */
    TEST_ASSERT_TRUE(can_core_parse_bytes("1002", buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX8(0x10, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[1]);
}

void test_parse_bytes_respects_max(void)
{
    uint8_t buf[2];
    size_t n = 0;

    /* must not overflow max_len */
    (void)can_core_parse_bytes("AABBCCDD", buf, sizeof(buf), &n);
    TEST_ASSERT_TRUE(n <= sizeof(buf));
}

/* ---- hex-id formatting -------------------------------------------------- */

void test_format_id_11(void)
{
    char buf[4] = { 0 };

    can_core_format_id_11(0x7E8, buf);
    TEST_ASSERT_EQUAL_STRING("7E8", buf);
}

void test_format_id_29(void)
{
    char buf[9] = { 0 };

    can_core_format_id_29(0x18DAF110, buf);
    TEST_ASSERT_EQUAL_STRING("18DAF110", buf);
}

/* ---- bus-off recovery restart policy ------------------------------------ */

void test_recovery_first_bus_off_restarts_immediately(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);
    TEST_ASSERT_EQUAL_UINT32(0, can_core_recovery_on_bus_off(&r, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, r.off_count);

    /* not armed until the hardware reports BUS_RECOVERED */
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, 6000));

    can_core_recovery_on_recovered(&r, 6000);
    TEST_ASSERT_TRUE(can_core_recovery_restart_due(&r, 6000));
    /* one-shot: consumed */
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, 7000));
}

void test_recovery_rapid_reoffense_escalates(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);

    /* thrash: bus-offs 2 s apart -> 0, 1 s, 2 s, 4 s ... capped at 30 s */
    uint32_t now = 1000;
    TEST_ASSERT_EQUAL_UINT32(0, can_core_recovery_on_bus_off(&r, now));
    now += 2000;
    TEST_ASSERT_EQUAL_UINT32(1000, can_core_recovery_on_bus_off(&r, now));
    now += 2000;
    TEST_ASSERT_EQUAL_UINT32(2000, can_core_recovery_on_bus_off(&r, now));
    now += 2000;
    TEST_ASSERT_EQUAL_UINT32(4000, can_core_recovery_on_bus_off(&r, now));

    for (int i = 0; i < 10; i++)
    {
        now += 2000;
        (void)can_core_recovery_on_bus_off(&r, now);
    }
    TEST_ASSERT_EQUAL_UINT32(CAN_CORE_RECOVERY_BACKOFF_MAX_MS,
                             r.backoff_ms);

    /* the armed restart honors the backoff window */
    can_core_recovery_on_recovered(&r, now);
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, now + 29999));
    TEST_ASSERT_TRUE(can_core_recovery_restart_due(&r, now + 30000));
}

void test_recovery_stability_resets_backoff(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);
    (void)can_core_recovery_on_bus_off(&r, 1000);
    (void)can_core_recovery_on_bus_off(&r, 2000);   /* -> 1 s   */
    (void)can_core_recovery_on_bus_off(&r, 3000);   /* -> 2 s   */

    /* >= 60 s of quiet -> next bus-off is treated as isolated again */
    uint32_t later = 3000 + CAN_CORE_RECOVERY_STABLE_MS;
    TEST_ASSERT_EQUAL_UINT32(0, can_core_recovery_on_bus_off(&r, later));
}

void test_recovery_new_bus_off_voids_armed_restart(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);
    (void)can_core_recovery_on_bus_off(&r, 1000);
    can_core_recovery_on_recovered(&r, 1500);

    /* bus went off again before we restarted: the pending restart must
     * not fire (twai_start in BUS_OFF state would be wrong) */
    (void)can_core_recovery_on_bus_off(&r, 1600);
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, 60000));

    can_core_recovery_on_recovered(&r, 61000);
    TEST_ASSERT_TRUE(can_core_recovery_restart_due(&r, 62001));
}

void test_recovery_spurious_recovered_ignored(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);
    can_core_recovery_on_recovered(&r, 1000);
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, 99999));
}

void test_recovery_retry_rearms(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);
    (void)can_core_recovery_on_bus_off(&r, 1000);
    can_core_recovery_on_recovered(&r, 2000);
    TEST_ASSERT_TRUE(can_core_recovery_restart_due(&r, 2000));

    /* twai_start failed -> retry path */
    can_core_recovery_retry(&r, 2000, 1000);
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, 2500));
    TEST_ASSERT_TRUE(can_core_recovery_restart_due(&r, 3000));
}

void test_recovery_wraparound_timestamps(void)
{
    can_core_recovery_t r;

    can_core_recovery_reset(&r);

    /* now near the uint32 wrap: policy math must survive the rollover */
    uint32_t now = 0xFFFFFC00u; /* ~1 s before wrap */
    (void)can_core_recovery_on_bus_off(&r, now);
    (void)can_core_recovery_on_bus_off(&r, now + 500); /* rapid -> 1 s */
    TEST_ASSERT_EQUAL_UINT32(1000, r.backoff_ms);

    can_core_recovery_on_recovered(&r, now + 600); /* due wraps past 0 */
    TEST_ASSERT_FALSE(can_core_recovery_restart_due(&r, now + 700));
    TEST_ASSERT_TRUE(can_core_recovery_restart_due(&r, now + 1700));
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_filter_match_exact);
    RUN_TEST(test_filter_match_pass_all);
    RUN_TEST(test_filter_match_partial_mask);
    RUN_TEST(test_filter_match_29bit);
    RUN_TEST(test_parse_id_11bit);
    RUN_TEST(test_parse_id_29bit);
    RUN_TEST(test_parse_id_masks_overflow);
    RUN_TEST(test_parse_id_rejects_non_hex);
    RUN_TEST(test_parse_byte);
    RUN_TEST(test_parse_bytes);
    RUN_TEST(test_parse_bytes_respects_max);
    RUN_TEST(test_format_id_11);
    RUN_TEST(test_format_id_29);

    RUN_TEST(test_recovery_first_bus_off_restarts_immediately);
    RUN_TEST(test_recovery_rapid_reoffense_escalates);
    RUN_TEST(test_recovery_stability_resets_backoff);
    RUN_TEST(test_recovery_new_bus_off_voids_armed_restart);
    RUN_TEST(test_recovery_spurious_recovered_ignored);
    RUN_TEST(test_recovery_wraparound_timestamps);
    RUN_TEST(test_recovery_retry_rearms);

    UNITY_END();
}
