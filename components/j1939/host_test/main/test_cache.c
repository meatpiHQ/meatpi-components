/**
 * @file test_cache.c
 * @brief Host suite for j1939_cache_core.c: the newest message per (group,
 *        source, destination), the pick among several sources, the bounds
 *        (table and long buffers) and the table of sources. Run from
 *        test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "j1939_cache_core.h"

#define S(x) ((int64_t)(x) * 1000000)
#define MS(x) ((int64_t)(x) * 1000)

#define EEC1  0x00F004u
#define CCVS1 0x00FEF1u
#define TSC1  0x000000u /* a PDU1 group: torque / speed control */

static j1939_cache_t s_c; /* 42 KB: not on the test's stack */
static j1939_sources_t s_src;

static const uint8_t A[8] = { 0xF0, 0xAA, 0xA5, 0xE0, 0x2E, 0xFF, 0xFF, 0xFF };
static const uint8_t B[8] = { 0xF0, 0xAA, 0xA5, 0x20, 0x4E, 0xFF, 0xFF, 0xFF };

/** Entries the walk finds: must equal `used`. */
static size_t walk_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < J1939_CACHE_SLOTS; i++)
    {
        n += (j1939_cache_at(&s_c, i) != NULL) ? 1u : 0u;
    }

    return n;
}

void test_cache_newest_count_and_period(void)
{
    const j1939_entry_t *e;

    j1939_cache_init(&s_c);
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, EEC1, 0, 0xFF, 0));

    e = j1939_cache_put(&s_c, EEC1, 0x00, 0xFF, A, 8, MS(100));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(1, e->count);
    TEST_ASSERT_EQUAL_UINT32(0, e->period_us);
    TEST_ASSERT_EQUAL_UINT32(1, s_c.seq);

    e = j1939_cache_put(&s_c, EEC1, 0x00, 0xFF, B, 8, MS(120));
    TEST_ASSERT_EQUAL_UINT32(2, e->count);
    TEST_ASSERT_EQUAL_UINT32(20000, e->period_us);
    TEST_ASSERT_EQUAL_UINT32(2, s_c.seq);
    TEST_ASSERT_EQUAL_UINT16(1, s_c.used);

    e = j1939_cache_get(&s_c, EEC1, 0x00, 0xFF, MS(121));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT16(8, e->len);
    TEST_ASSERT_EQUAL_INT64(MS(120), e->rx_us);
    TEST_ASSERT_EQUAL_MEMORY(B, j1939_cache_payload(&s_c, e), 8);

    /* another source, another group: their own entries */
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, EEC1, 0x11, 0xFF, MS(121)));
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, CCVS1, 0x00, 0xFF, MS(121)));
    TEST_ASSERT_EQUAL_UINT32(1, walk_count());
}

void test_cache_short_frames_and_empty_payload(void)
{
    static const uint8_t REQ[3] = { 0xEC, 0xFE, 0x00 };
    const j1939_entry_t *e;

    j1939_cache_init(&s_c);
    e = j1939_cache_put(&s_c, J1939_PGN_REQUEST, 0xF9, 0x00, REQ, 3, 1);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT16(3, e->len);
    TEST_ASSERT_EQUAL_MEMORY(REQ, j1939_cache_payload(&s_c, e), 3);

    /* a frame without data is stored, and a lookup does not return it */
    e = j1939_cache_put(&s_c, 0x00FF10u, 0x21, 0xFF, NULL, 0, 2);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(1, e->count);
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, 0x00FF10u, 0x21, 0xFF, 3));

    /* what cannot be stored */
    TEST_ASSERT_NULL(j1939_cache_put(&s_c, EEC1, 0, 0xFF, NULL, 8, 4));
    TEST_ASSERT_NULL(j1939_cache_put(&s_c, EEC1, 0, 0xFF, A,
                                     (uint16_t)(J1939_MSG_MAX + 1u), 4));
    TEST_ASSERT_NULL(j1939_cache_put(&s_c, J1939_PGN_FREE, 0, 0xFF, A, 8, 4));
    TEST_ASSERT_EQUAL_UINT16(2, s_c.used);
}

void test_cache_pick_lowest_fresh_source(void)
{
    const j1939_entry_t *e;

    j1939_cache_init(&s_c);
    /* the engine (00) and a second controller (11) both send EEC1 */
    j1939_cache_put(&s_c, EEC1, 0x11, 0xFF, B, 8, S(10));
    j1939_cache_put(&s_c, EEC1, 0x00, 0xFF, A, 8, S(10) + MS(5));

    e = j1939_cache_get(&s_c, EEC1, J1939_ADDR_ANY, J1939_ADDR_ANY, S(10) + MS(6));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8(0x00, e->sa);

    /* the higher address spoke last: the pick stays on the lowest */
    j1939_cache_put(&s_c, EEC1, 0x11, 0xFF, B, 8, S(10) + MS(20));
    e = j1939_cache_get(&s_c, EEC1, J1939_ADDR_ANY, J1939_ADDR_ANY, S(10) + MS(21));
    TEST_ASSERT_EQUAL_HEX8(0x00, e->sa);

    /* the engine falls silent, the other keeps sending: after
       J1939_FRESH_US the pick moves */
    j1939_cache_put(&s_c, EEC1, 0x11, 0xFF, B, 8, S(16));
    e = j1939_cache_get(&s_c, EEC1, J1939_ADDR_ANY, J1939_ADDR_ANY, S(16));
    TEST_ASSERT_EQUAL_HEX8(0x11, e->sa);

    /* everybody silent for long: the lowest source, not whoever spoke
       last (a pick that moves with the clock makes stale news) */
    e = j1939_cache_get(&s_c, EEC1, J1939_ADDR_ANY, J1939_ADDR_ANY, S(600));
    TEST_ASSERT_EQUAL_HEX8(0x00, e->sa);

    /* a named source is that source, fresh or not */
    e = j1939_cache_get(&s_c, EEC1, 0x00, J1939_ADDR_ANY, S(600));
    TEST_ASSERT_EQUAL_HEX8(0x00, e->sa);
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, EEC1, 0x22, J1939_ADDR_ANY, S(600)));
}

void test_cache_pdu1_destination_is_part_of_the_key(void)
{
    const j1939_entry_t *e;

    j1939_cache_init(&s_c);
    /* the same group from one source to two destinations */
    j1939_cache_put(&s_c, TSC1, 0x0B, 0x00, A, 8, S(1));
    j1939_cache_put(&s_c, TSC1, 0x0B, 0x0F, B, 8, S(1) + 1);
    TEST_ASSERT_EQUAL_UINT16(2, s_c.used);

    e = j1939_cache_get(&s_c, TSC1, 0x0B, 0x00, S(1) + 2);
    TEST_ASSERT_EQUAL_MEMORY(A, j1939_cache_payload(&s_c, e), 8);
    e = j1939_cache_get(&s_c, TSC1, 0x0B, 0x0F, S(1) + 2);
    TEST_ASSERT_EQUAL_MEMORY(B, j1939_cache_payload(&s_c, e), 8);

    /* any destination of that source: one of them, and the same one twice */
    e = j1939_cache_get(&s_c, TSC1, 0x0B, J1939_ADDR_ANY, S(1) + 2);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_PTR(e, j1939_cache_get(&s_c, TSC1, 0x0B, J1939_ADDR_ANY,
                                             S(1) + 3));
}

void test_cache_long_messages(void)
{
    static uint8_t big[J1939_MSG_MAX];
    const j1939_entry_t *e;

    for (size_t i = 0; i < sizeof(big); i++)
    {
        big[i] = (uint8_t)(i ^ 0x5Au);
    }

    j1939_cache_init(&s_c);
    e = j1939_cache_put(&s_c, 0x00FF00u, 0x21, 0xFF, big, J1939_MSG_MAX, S(1));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT16(J1939_MSG_MAX, e->len);
    TEST_ASSERT_EQUAL_MEMORY(big, j1939_cache_payload(&s_c, e), J1939_MSG_MAX);

    /* the next one of the same key reuses its buffer */
    big[0] = 0x77;
    e = j1939_cache_put(&s_c, 0x00FF00u, 0x21, 0xFF, big, 100, S(2));
    TEST_ASSERT_EQUAL_UINT16(100, e->len);
    TEST_ASSERT_EQUAL_HEX8(0x77, j1939_cache_payload(&s_c, e)[0]);
    TEST_ASSERT_EQUAL_UINT32(0, s_c.lng_evicted);

    /* back to a frame's worth (DM1 with one code again): the buffer is
       given back */
    e = j1939_cache_put(&s_c, 0x00FF00u, 0x21, 0xFF, A, 8, S(3));
    TEST_ASSERT_EQUAL_INT8(-1, e->lng);
    TEST_ASSERT_EQUAL_MEMORY(A, j1939_cache_payload(&s_c, e), 8);

    for (size_t i = 0; i < J1939_LONG_SLOTS; i++)
    {
        TEST_ASSERT_EQUAL_INT16(-1, s_c.lng_owner[i]);
    }
}

void test_cache_long_buffers_run_out(void)
{
    static uint8_t msg[40];
    const j1939_entry_t *e;

    j1939_cache_init(&s_c);

    /* one long message from each of J1939_LONG_SLOTS sources */
    for (unsigned sa = 0; sa < J1939_LONG_SLOTS; sa++)
    {
        memset(msg, (int)sa, sizeof(msg));
        TEST_ASSERT_NOT_NULL(j1939_cache_put(&s_c, J1939_PGN_DM1, (uint8_t)sa,
                                             0xFF, msg, sizeof(msg),
                                             S(1) + sa));
    }

    TEST_ASSERT_EQUAL_UINT32(0, s_c.lng_evicted);

    /* one more: the holder that has been silent the longest (source 0)
       loses its payload, its entry stays */
    memset(msg, 0xEE, sizeof(msg));
    e = j1939_cache_put(&s_c, J1939_PGN_DM1, 0x50, 0xFF, msg, sizeof(msg), S(2));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8(0xEE, j1939_cache_payload(&s_c, e)[39]);
    TEST_ASSERT_EQUAL_UINT32(1, s_c.lng_evicted);
    TEST_ASSERT_EQUAL_UINT16(J1939_LONG_SLOTS + 1u, s_c.used);
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, J1939_PGN_DM1, 0x00, 0xFF, S(2)));

    /* source 1 still has its own */
    e = j1939_cache_get(&s_c, J1939_PGN_DM1, 0x01, 0xFF, S(2));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8(0x01, j1939_cache_payload(&s_c, e)[0]);

    /* source 0 sends again: it is stored again (and takes the next
       longest-silent holder's buffer, source 1) */
    memset(msg, 0x10, sizeof(msg));
    e = j1939_cache_put(&s_c, J1939_PGN_DM1, 0x00, 0xFF, msg, sizeof(msg), S(3));
    TEST_ASSERT_EQUAL_UINT32(2, e->count);
    TEST_ASSERT_EQUAL_HEX8(0x10, j1939_cache_payload(&s_c, e)[0]);
    TEST_ASSERT_EQUAL_UINT32(2, s_c.lng_evicted);
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, J1939_PGN_DM1, 0x01, 0xFF, S(3)));
}

void test_cache_full_table_and_eviction(void)
{
    const j1939_entry_t *e;
    uint8_t d[8] = { 0 };

    j1939_cache_init(&s_c);

    /* fill it: J1939_CACHE_MAX distinct keys, all at t = 1 s .. */
    for (unsigned i = 0; i < J1939_CACHE_MAX; i++)
    {
        d[0] = (uint8_t)i;
        d[1] = (uint8_t)(i >> 8);
        TEST_ASSERT_NOT_NULL(j1939_cache_put(&s_c, 0x00FF00u + (i >> 8),
                                             (uint8_t)i, 0xFF, d, 8,
                                             S(1) + i));
    }

    TEST_ASSERT_EQUAL_UINT16(J1939_CACHE_MAX, s_c.used);
    TEST_ASSERT_EQUAL_UINT32(J1939_CACHE_MAX, walk_count());

    /* a new key while everything is recent: not kept, counted */
    TEST_ASSERT_NULL(j1939_cache_put(&s_c, EEC1, 0x00, 0xFF, A, 8, S(2)));
    TEST_ASSERT_EQUAL_UINT32(1, s_c.full);
    TEST_ASSERT_EQUAL_UINT32(0, s_c.evicted);

    /* known keys still update */
    d[0] = 0x99;
    e = j1939_cache_put(&s_c, 0x00FF00u, 0x05, 0xFF, d, 8, S(2));
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(2, e->count);

    /* later the oldest entry (key 0, t = 1 s) has been silent long enough:
       it makes room */
    e = j1939_cache_put(&s_c, EEC1, 0x00, 0xFF, A, 8, S(1) + J1939_EVICT_AGE_US);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(1, s_c.evicted);
    TEST_ASSERT_EQUAL_UINT16(J1939_CACHE_MAX, s_c.used);
    TEST_ASSERT_EQUAL_UINT32(J1939_CACHE_MAX, walk_count());
    TEST_ASSERT_NULL(j1939_cache_get(&s_c, 0x00FF00u, 0x00, 0xFF, S(40)));

    /* every other key is still found, with its own payload: removal did
       not break a probe chain */
    for (unsigned i = 1; i < J1939_CACHE_MAX; i++)
    {
        e = j1939_cache_get(&s_c, 0x00FF00u + (i >> 8), (int)(i & 0xFFu), 0xFF,
                            S(40));
        TEST_ASSERT_NOT_NULL(e);

        if (i != 5)
        {
            TEST_ASSERT_EQUAL_HEX8((uint8_t)i, j1939_cache_payload(&s_c, e)[0]);
            TEST_ASSERT_EQUAL_HEX8((uint8_t)(i >> 8),
                                   j1939_cache_payload(&s_c, e)[1]);
        }
    }

    e = j1939_cache_get(&s_c, EEC1, 0x00, 0xFF, S(40));
    TEST_ASSERT_EQUAL_MEMORY(A, j1939_cache_payload(&s_c, e), 8);
}

void test_cache_eviction_keeps_long_buffers_with_their_owner(void)
{
    static uint8_t msg[60];
    const j1939_entry_t *e;
    uint8_t d[8] = { 0 };

    j1939_cache_init(&s_c);

    /* key 0 is the oldest; a long message sits somewhere in the middle */
    for (unsigned i = 0; i < J1939_CACHE_MAX - 1u; i++)
    {
        d[0] = (uint8_t)i;
        j1939_cache_put(&s_c, 0x00FF00u + (i >> 8), (uint8_t)i, 0xFF, d, 8,
                        S(1) + i);
    }

    memset(msg, 0xAB, sizeof(msg));
    j1939_cache_put(&s_c, J1939_PGN_VIN, 0x00, 0xFF, msg, sizeof(msg), S(5));
    TEST_ASSERT_EQUAL_UINT16(J1939_CACHE_MAX, s_c.used);

    /* evict again and again: entries move in the table, the long message
       must stay readable under its key */
    for (unsigned n = 0; n < 40; n++)
    {
        TEST_ASSERT_NOT_NULL(j1939_cache_put(&s_c, 0x00FE00u + n, 0x80, 0xFF,
                                             d, 8, S(100) + n));
        /* keep the long one young */
        j1939_cache_put(&s_c, J1939_PGN_VIN, 0x00, 0xFF, msg, sizeof(msg),
                        S(100) + n);
        e = j1939_cache_get(&s_c, J1939_PGN_VIN, 0x00, 0xFF, S(100) + n);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT16(sizeof(msg), e->len);
        TEST_ASSERT_EQUAL_HEX8(0xAB, j1939_cache_payload(&s_c, e)[59]);
        TEST_ASSERT_EQUAL_INT16((int16_t)(e - s_c.e), s_c.lng_owner[e->lng]);
    }

    TEST_ASSERT_EQUAL_UINT32(40, s_c.evicted);
    TEST_ASSERT_EQUAL_UINT32(J1939_CACHE_MAX, walk_count());
}

void test_sources(void)
{
    static const uint8_t NAME[8] = { 0xB3, 0xA2, 0x61, 0x24, 0, 0, 0, 0 };
    uint8_t got[8] = { 0 };

    j1939_sources_init(&s_src);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_sources_count(&s_src));

    j1939_sources_note(&s_src, 0x00, MS(1));
    j1939_sources_note(&s_src, 0x00, MS(2));
    j1939_sources_note(&s_src, 0xFE, MS(3));
    TEST_ASSERT_EQUAL_UINT32(2, j1939_sources_count(&s_src));
    TEST_ASSERT_EQUAL_UINT32(2, s_src.frames[0x00]);
    TEST_ASSERT_EQUAL_INT64(MS(2), s_src.last_us[0x00]);

    TEST_ASSERT_FALSE(j1939_sources_name(&s_src, 0x00, got));
    j1939_sources_claim(&s_src, 0x00, NAME);
    TEST_ASSERT_TRUE(j1939_sources_name(&s_src, 0x00, got));
    TEST_ASSERT_EQUAL_MEMORY(NAME, got, 8);
    TEST_ASSERT_TRUE(j1939_sources_name(&s_src, 0x00, NULL));
    TEST_ASSERT_FALSE(j1939_sources_name(&s_src, 0xFE, NULL));
}

void run_cache_tests(void)
{
    RUN_TEST(test_cache_newest_count_and_period);
    RUN_TEST(test_cache_short_frames_and_empty_payload);
    RUN_TEST(test_cache_pick_lowest_fresh_source);
    RUN_TEST(test_cache_pdu1_destination_is_part_of_the_key);
    RUN_TEST(test_cache_long_messages);
    RUN_TEST(test_cache_long_buffers_run_out);
    RUN_TEST(test_cache_full_table_and_eviction);
    RUN_TEST(test_cache_eviction_keeps_long_buffers_with_their_owner);
    RUN_TEST(test_sources);
}
