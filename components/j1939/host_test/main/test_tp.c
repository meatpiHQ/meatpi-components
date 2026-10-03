/**
 * @file test_tp.c
 * @brief Host suite for j1939_tp_core.c: transport protocol reassembly as a
 *        listener sees it. Frames are the ones the bench truck sends
 *        (tools/testbench/lib/j1939_ref.py): the VIN by BAM, a three-code
 *        DM1 by RTS/CTS. Run from test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "j1939_tp_core.h"

#define MS(x) ((int64_t)(x) * 1000)

static j1939_tp_t s_tp; /* 14.5 KB: not on the test's stack */

/* VIN "1WCANJ1939TRUCK01*", 18 bytes, 3 packets */
static const uint8_t VIN_BAM[8] = { 0x20, 0x12, 0x00, 0x03, 0xFF, 0xEC, 0xFE, 0x00 };
static const uint8_t VIN_DT[3][8] =
{
    { 0x01, 0x31, 0x57, 0x43, 0x41, 0x4E, 0x4A, 0x31 },
    { 0x02, 0x39, 0x33, 0x39, 0x54, 0x52, 0x55, 0x43 },
    { 0x03, 0x4B, 0x30, 0x31, 0x2A, 0xFF, 0xFF, 0xFF },
};

/* DM1 with three trouble codes, 14 bytes, 2 packets, engine -> F9 */
static const uint8_t DM1_RTS[8] = { 0x10, 0x0E, 0x00, 0x02, 0xFF, 0xCA, 0xFE, 0x00 };
static const uint8_t DM1_CTS[8] = { 0x11, 0x02, 0x01, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
static const uint8_t DM1_EOMA[8] = { 0x13, 0x0E, 0x00, 0x02, 0xFF, 0xCA, 0xFE, 0x00 };
static const uint8_t DM1_ABORT[8] = { 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
static const uint8_t DM1_DT[2][8] =
{
    { 0x01, 0x44, 0xFF, 0x6E, 0x00, 0x00, 0x05, 0x9A },
    { 0x02, 0x0C, 0x04, 0x01, 0x00, 0xF0, 0xFF, 0x7E },
};
static const uint8_t DM1_MSG[14] =
{
    0x44, 0xFF, 0x6E, 0x00, 0x00, 0x05, 0x9A,
    0x0C, 0x04, 0x01, 0x00, 0xF0, 0xFF, 0x7E,
};

/** started == every way a session can end + the open ones. */
static void books_balance(void)
{
    const j1939_tp_stats_t *st = &s_tp.stats;

    TEST_ASSERT_EQUAL_UINT32(st->started,
                             st->completed + st->seq_errors + st->timeouts +
                                 st->aborted + st->replaced +
                                 (uint32_t)j1939_tp_open(&s_tp));
}

void test_tp_bam_vin(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(0));
    TEST_ASSERT_EQUAL_UINT32(1, j1939_tp_open(&s_tp));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 8, MS(55), &m));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[1], 8, MS(110), &m));
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[2], 8, MS(165), &m));

    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_VIN, m.pgn);
    TEST_ASSERT_EQUAL_HEX8(0x00, m.sa);
    TEST_ASSERT_EQUAL_HEX8(0xFF, m.da);
    TEST_ASSERT_EQUAL_UINT16(18, m.len);
    TEST_ASSERT_EQUAL_MEMORY("1WCANJ1939TRUCK01*", m.data, 18);

    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.started);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.completed);
    books_balance();
}

void test_tp_bam_dropped_packet(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 8, MS(55), &m));
    /* packet 2 never came: nothing is handed out, the session is over */
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[2], 8, MS(165), &m));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.seq_errors);
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.completed);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));
    books_balance();
}

void test_tp_bam_reordered_packets(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[1], 8, MS(55), &m));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.seq_errors);
    /* what follows belongs to nothing */
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 8, MS(110), &m));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[2], 8, MS(165), &m));
    TEST_ASSERT_EQUAL_UINT32(2, s_tp.stats.orphan_dt);
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.completed);
    books_balance();
}

void test_tp_bam_stall_times_out(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 8, MS(55), &m));

    j1939_tp_expire(&s_tp, MS(55) + J1939_TP_TIMEOUT_US);
    TEST_ASSERT_EQUAL_UINT32(1, j1939_tp_open(&s_tp)); /* not yet */
    j1939_tp_expire(&s_tp, MS(55) + J1939_TP_TIMEOUT_US + 1);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.timeouts);

    /* the late packet is an orphan */
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[1], 8, MS(3000), &m));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.orphan_dt);
    books_balance();
}

void test_tp_new_announce_replaces_the_open_one(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 8, MS(55), &m));
    /* the sender starts over */
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(100));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.replaced);
    TEST_ASSERT_EQUAL_UINT32(1, j1939_tp_open(&s_tp));

    for (int i = 0; i < 2; i++)
    {
        TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[i], 8,
                                      MS(150 + 50 * i), &m));
    }

    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[2], 8, MS(250), &m));
    TEST_ASSERT_EQUAL_MEMORY("1WCANJ1939TRUCK01*", m.data, 18);
    books_balance();
}

void test_tp_rts_cts_between_two_other_nodes(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    /* the engine (00) to a tool (F9): the listener is neither */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(0));
    j1939_tp_cm(&s_tp, 0xF9, 0x00, DM1_CTS, 8, MS(5));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(8), &m));
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[1], 8, MS(11), &m));
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_DM1, m.pgn);
    TEST_ASSERT_EQUAL_HEX8(0x00, m.sa);
    TEST_ASSERT_EQUAL_HEX8(0xF9, m.da);
    TEST_ASSERT_EQUAL_UINT16(sizeof(DM1_MSG), m.len);
    TEST_ASSERT_EQUAL_MEMORY(DM1_MSG, m.data, sizeof(DM1_MSG));

    /* the receiver's acknowledge finds nothing left to close */
    j1939_tp_cm(&s_tp, 0xF9, 0x00, DM1_EOMA, 8, MS(14));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.completed);
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.seq_errors);
    books_balance();
}

void test_tp_cts_asks_for_a_packet_again(void)
{
    static const uint8_t CTS_AGAIN[8] =
        { 0x11, 0x01, 0x01, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
    static const uint8_t CTS_HOLD[8] =
        { 0x11, 0x00, 0xFF, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(0));
    j1939_tp_cm(&s_tp, 0xF9, 0x00, DM1_CTS, 8, MS(5));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(8), &m));

    /* a hold keeps the session alive past the timeout */
    j1939_tp_cm(&s_tp, 0xF9, 0x00, CTS_HOLD, 8, MS(1000));
    j1939_tp_expire(&s_tp, MS(2000));
    TEST_ASSERT_EQUAL_UINT32(1, j1939_tp_open(&s_tp));

    /* the receiver did not get packet 1: the sender repeats from there */
    j1939_tp_cm(&s_tp, 0xF9, 0x00, CTS_AGAIN, 8, MS(2010));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(2013), &m));
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[1], 8, MS(2016), &m));
    TEST_ASSERT_EQUAL_MEMORY(DM1_MSG, m.data, sizeof(DM1_MSG));
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.seq_errors);
    books_balance();
}

void test_tp_abort_and_acknowledge_of_an_incomplete_session(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);

    /* abort by the sender */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(0));
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_ABORT, 8, MS(5));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.aborted);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));

    /* abort by the receiver */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(100));
    j1939_tp_cm(&s_tp, 0xF9, 0x00, DM1_ABORT, 8, MS(105));
    TEST_ASSERT_EQUAL_UINT32(2, s_tp.stats.aborted);

    /* an abort for nothing that is open changes nothing */
    j1939_tp_cm(&s_tp, 0xF9, 0x00, DM1_ABORT, 8, MS(110));
    TEST_ASSERT_EQUAL_UINT32(2, s_tp.stats.aborted);

    /* the receiver acknowledges a message this listener missed part of */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(200));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(205), &m));
    j1939_tp_cm(&s_tp, 0xF9, 0x00, DM1_EOMA, 8, MS(210));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.seq_errors);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));

    /* a CTS that is ahead of what was seen here: packets were lost */
    static const uint8_t CTS_AHEAD[8] =
        { 0x11, 0x01, 0x02, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };

    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(300));
    j1939_tp_cm(&s_tp, 0xF9, 0x00, CTS_AHEAD, 8, MS(305));
    TEST_ASSERT_EQUAL_UINT32(2, s_tp.stats.seq_errors);
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.completed);
    books_balance();
}

void test_tp_bad_announces(void)
{
    uint8_t cm[8];

    j1939_tp_init(&s_tp);

    /* 8 bytes do not need the transport protocol */
    memcpy(cm, VIN_BAM, 8);
    cm[1] = 8;
    cm[3] = 2;
    j1939_tp_cm(&s_tp, 0x00, 0xFF, cm, 8, 0);

    /* more than the protocol carries */
    cm[1] = 0xFA;
    cm[2] = 0x06; /* 1786 */
    cm[3] = 0xFF;
    j1939_tp_cm(&s_tp, 0x00, 0xFF, cm, 8, 0);

    /* a packet count that does not match the size */
    memcpy(cm, VIN_BAM, 8);
    cm[3] = 4;
    j1939_tp_cm(&s_tp, 0x00, 0xFF, cm, 8, 0);

    /* a BAM to one node, an RTS to everyone, a short frame, a control
       byte nobody defined */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, VIN_BAM, 8, 0);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, DM1_RTS, 8, 0);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 7, 0);
    memcpy(cm, VIN_BAM, 8);
    cm[0] = 0x55;
    j1939_tp_cm(&s_tp, 0x00, 0xFF, cm, 8, 0);

    TEST_ASSERT_EQUAL_UINT32(7, s_tp.stats.bad_cm);
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.started);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));
}

void test_tp_largest_message(void)
{
    static uint8_t want[J1939_MSG_MAX];
    uint8_t cm[8] = { 0x20, 0xF9, 0x06, 0xFF, 0xFF, 0x00, 0xFF, 0x00 };
    j1939_tp_msg_t m = { 0 };
    bool done = false;

    for (size_t i = 0; i < sizeof(want); i++)
    {
        want[i] = (uint8_t)(i * 7u + 3u);
    }

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x21, 0xFF, cm, 8, 0); /* 1785 bytes, 255 packets */
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.started);

    for (unsigned seq = 1; seq <= 255; seq++)
    {
        uint8_t dt[8];

        dt[0] = (uint8_t)seq;
        memcpy(&dt[1], &want[(seq - 1u) * 7u], 7);
        TEST_ASSERT_FALSE(done);
        done = j1939_tp_dt(&s_tp, 0x21, 0xFF, dt, 8, MS(seq * 50), &m);
    }

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT16(J1939_MSG_MAX, m.len);
    TEST_ASSERT_EQUAL_HEX32(0x00FF00u, m.pgn);
    TEST_ASSERT_EQUAL_MEMORY(want, m.data, sizeof(want));
    books_balance();
}

void test_tp_sessions_side_by_side_and_the_pool_limit(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);

    /* every session slot taken by a different sender */
    for (unsigned sa = 0; sa < J1939_TP_SESSIONS; sa++)
    {
        j1939_tp_cm(&s_tp, (uint8_t)sa, 0xFF, VIN_BAM, 8, 0);
    }

    TEST_ASSERT_EQUAL_UINT32(J1939_TP_SESSIONS, j1939_tp_open(&s_tp));

    /* one more: counted, not started */
    j1939_tp_cm(&s_tp, 0x40, 0xFF, VIN_BAM, 8, 0);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.no_session);
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x40, 0xFF, VIN_DT[0], 8, 1, &m));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.orphan_dt);

    /* the packets of the eight interleave: each message comes out whole */
    for (int p = 0; p < 3; p++)
    {
        for (unsigned sa = 0; sa < J1939_TP_SESSIONS; sa++)
        {
            bool done = j1939_tp_dt(&s_tp, (uint8_t)sa, 0xFF, VIN_DT[p], 8,
                                    MS(50 * (p + 1)), &m);

            TEST_ASSERT_EQUAL(p == 2, done);

            if (done)
            {
                TEST_ASSERT_EQUAL_HEX8(sa, m.sa);
                TEST_ASSERT_EQUAL_MEMORY("1WCANJ1939TRUCK01*", m.data, 18);
            }
        }
    }

    TEST_ASSERT_EQUAL_UINT32(J1939_TP_SESSIONS, s_tp.stats.completed);

    /* a BAM and a connection of the same sender do not collide */
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, MS(1000));
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(1000));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(1001), &m));
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 8, MS(1050), &m));
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[1], 8, MS(1051), &m));
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_DM1, m.pgn);
    TEST_ASSERT_EQUAL_UINT32(1, j1939_tp_open(&s_tp));
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.seq_errors);
    books_balance();
}

void test_tp_short_data_packet_ends_the_session(void)
{
    j1939_tp_msg_t m = { 0 };

    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xFF, VIN_BAM, 8, 0);
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xFF, VIN_DT[0], 5, MS(50), &m));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.seq_errors);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));
    books_balance();
}

void run_tp_tests(void)
{
    RUN_TEST(test_tp_bam_vin);
    RUN_TEST(test_tp_bam_dropped_packet);
    RUN_TEST(test_tp_bam_reordered_packets);
    RUN_TEST(test_tp_bam_stall_times_out);
    RUN_TEST(test_tp_new_announce_replaces_the_open_one);
    RUN_TEST(test_tp_rts_cts_between_two_other_nodes);
    RUN_TEST(test_tp_cts_asks_for_a_packet_again);
    RUN_TEST(test_tp_abort_and_acknowledge_of_an_incomplete_session);
    RUN_TEST(test_tp_bad_announces);
    RUN_TEST(test_tp_largest_message);
    RUN_TEST(test_tp_sessions_side_by_side_and_the_pool_limit);
    RUN_TEST(test_tp_short_data_packet_ends_the_session);
}
