/**
 * @file test_active.c
 * @brief Host suite for the active-mode cores (TASK_j1939_wwh.md phase 6):
 *        the NAME and the address claim (j1939_claim_core.c), the request
 *        and acknowledgment codec (j1939_core.c), and the transport protocol
 *        as a DESTINATION (j1939_tp_core.c: clear-to-send, end-of-message,
 *        aborts). Frames are the ones tools/testbench/lib/j1939_ref.py and
 *        the bench truck produce. Run from test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "j1939_claim_core.h"
#include "j1939_core.h"
#include "j1939_tp_core.h"

#define MS(x) ((int64_t)(x) * 1000)

static j1939_tp_t s_tp; /* 14.5 KB: not on the test's stack */

/* ---- NAME --------------------------------------------------------------------- */

void test_name_build_matches_the_reference(void)
{
    uint8_t name[8];

    /* j1939_ref.name_field(0x1A2B3, manufacturer=0x123, function=0): the
       truck's NAME, identity in the low 21 bits, manufacturer above it */
    j1939_name_build(0x1A2B3, 0x123, 0, false, name);
    TEST_ASSERT_EQUAL_HEX8(0xB3, name[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA2, name[1]);
    TEST_ASSERT_EQUAL_HEX8(0x61, name[2]); /* 0x1A2B3 >> 16 = 1 | 0x123 << 5 */
    TEST_ASSERT_EQUAL_HEX8(0x24, name[3]); /* 0x123 >> 3                     */
    TEST_ASSERT_EQUAL_HEX8(0x00, name[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, name[5]);
    TEST_ASSERT_EQUAL_HEX8(0x00, name[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, name[7]);

    /* this device: a service tool, arbitrary address capable */
    j1939_name_build(0x1FFFFF, 0, J1939_FUNCTION_TOOL, true, name);
    TEST_ASSERT_EQUAL_HEX8(0xFF, name[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, name[1]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, name[2]); /* identity ends at bit 20       */
    TEST_ASSERT_EQUAL_HEX8(0x00, name[3]);
    TEST_ASSERT_EQUAL_HEX8(0x81, name[5]); /* function 129                   */
    TEST_ASSERT_EQUAL_HEX8(0x80, name[7]); /* arbitrary address capable     */
}

void test_name_compare_lower_wins(void)
{
    uint8_t ours[8], zeros[8], ffs[8], other[8];

    j1939_name_build(0x12345, 0, J1939_FUNCTION_TOOL, true, ours);
    memset(zeros, 0x00, 8);
    memset(ffs, 0xFF, 8);
    TEST_ASSERT_TRUE(j1939_name_compare(zeros, ours) < 0);  /* zeros win   */
    TEST_ASSERT_TRUE(j1939_name_compare(ours, zeros) > 0);
    TEST_ASSERT_TRUE(j1939_name_compare(ours, ffs) < 0);    /* we beat FF  */
    TEST_ASSERT_EQUAL_INT(0, j1939_name_compare(ours, ours));

    /* the most significant byte decides before the identity does */
    j1939_name_build(0x00001, 0, J1939_FUNCTION_TOOL, true, other);
    TEST_ASSERT_TRUE(j1939_name_compare(other, ours) < 0);
    other[7] = 0x00; /* not arbitrary capable: a lower number */
    other[0] = 0xFF;
    TEST_ASSERT_TRUE(j1939_name_compare(other, ours) < 0);
}

/* ---- the claim ------------------------------------------------------------------ */

static j1939_claim_t s_claim;
static uint8_t s_name[8];

static void claim_fresh(void)
{
    j1939_name_build(0x12345, 0, J1939_FUNCTION_TOOL, true, s_name);
    j1939_claim_init(&s_claim, s_name, J1939_CLAIM_TOOL_1);
}

void test_claim_preferred_address_after_the_wait(void)
{
    j1939_claim_out_t out;

    claim_fresh();
    TEST_ASSERT_EQUAL_INT(J1939_CLAIM_IDLE, s_claim.state);
    TEST_ASSERT_FALSE(j1939_claim_ready(&s_claim));
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_NULL, s_claim.sa);

    j1939_claim_start(&s_claim, MS(1000), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(0xF9, out.sa);
    TEST_ASSERT_EQUAL_MEMORY(s_name, out.data, 8);
    TEST_ASSERT_EQUAL_INT(J1939_CLAIM_CLAIMING, s_claim.state);
    TEST_ASSERT_FALSE(j1939_claim_ready(&s_claim));

    j1939_claim_tick(&s_claim, MS(1249));
    TEST_ASSERT_FALSE(j1939_claim_ready(&s_claim));
    j1939_claim_tick(&s_claim, MS(1250));
    TEST_ASSERT_TRUE(j1939_claim_ready(&s_claim));
    TEST_ASSERT_EQUAL_HEX8(0xF9, s_claim.sa);
    TEST_ASSERT_EQUAL_UINT32(1, s_claim.stats.claims_sent);
    TEST_ASSERT_EQUAL_STRING("claimed",
                             j1939_claim_state_name(s_claim.state));
}

void test_claim_contest_won_defends_without_a_new_wait(void)
{
    j1939_claim_out_t out;
    uint8_t ffs[8];

    memset(ffs, 0xFF, 8);
    claim_fresh();
    j1939_claim_start(&s_claim, 0, &out);
    j1939_claim_tick(&s_claim, MS(300));
    TEST_ASSERT_TRUE(j1939_claim_ready(&s_claim));

    /* the truck's --contend 0xF9:lose: a claim of our address with an
       all-FF NAME, which loses to anything */
    j1939_claim_rx(&s_claim, 0xF9, ffs, MS(400), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(0xF9, out.sa);
    TEST_ASSERT_EQUAL_MEMORY(s_name, out.data, 8);
    TEST_ASSERT_TRUE(j1939_claim_ready(&s_claim)); /* kept, no second wait */
    TEST_ASSERT_EQUAL_UINT32(1, s_claim.stats.contests);
    TEST_ASSERT_EQUAL_UINT32(1, s_claim.stats.won);
    TEST_ASSERT_EQUAL_UINT32(2, s_claim.stats.claims_sent);

    /* somebody else's address: none of our business */
    j1939_claim_rx(&s_claim, 0x00, ffs, MS(500), &out);
    TEST_ASSERT_FALSE(out.send);
    TEST_ASSERT_EQUAL_UINT32(1, s_claim.stats.contests);
}

void test_claim_defence_once_per_round(void)
{
    j1939_claim_out_t out;
    uint8_t ffs[8];
    unsigned sent = 0;

    memset(ffs, 0xFF, 8);
    claim_fresh();
    j1939_claim_start(&s_claim, 0, &out);
    j1939_claim_tick(&s_claim, MS(300));

    /* a broken node contests every claim it hears, at wire speed (the bench
       truck's contend mode: 150 rounds in 2 s): one defence per 250 ms */
    for (unsigned i = 0; i < 100; i++)
    {
        j1939_claim_rx(&s_claim, 0xF9, ffs, MS(400) + (int64_t)i * 10000, &out);
        sent += out.send ? 1 : 0;
    }

    TEST_ASSERT_EQUAL_UINT32(100, s_claim.stats.contests);
    TEST_ASSERT_EQUAL_UINT32(100, s_claim.stats.won);
    TEST_ASSERT_EQUAL_UINT(4, sent); /* at 400, 650, 900, 1150 ms */
    TEST_ASSERT_EQUAL_UINT32(96, s_claim.stats.held);
    TEST_ASSERT_EQUAL_UINT32(5, s_claim.stats.claims_sent);
    TEST_ASSERT_TRUE(j1939_claim_ready(&s_claim));

    /* a quiet quarter second later the next contest is answered again */
    j1939_claim_rx(&s_claim, 0xF9, ffs, MS(1700), &out);
    TEST_ASSERT_TRUE(out.send);

    /* a move starts a new round: the first contest there is answered */
    uint8_t zeros[8];

    memset(zeros, 0x00, 8);
    j1939_claim_rx(&s_claim, 0xF9, zeros, MS(1710), &out);
    TEST_ASSERT_EQUAL_HEX8(0xFA, out.sa);
    j1939_claim_rx(&s_claim, 0xFA, ffs, MS(1720), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(0xFA, out.sa);
}

void test_claim_contest_lost_moves_to_the_next_address(void)
{
    j1939_claim_out_t out;
    uint8_t zeros[8];

    memset(zeros, 0x00, 8);
    claim_fresh();
    j1939_claim_start(&s_claim, 0, &out);

    /* --contend 0xF9:win while we are still waiting: we move to tool 2 */
    j1939_claim_rx(&s_claim, 0xF9, zeros, MS(100), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(0xFA, out.sa);
    TEST_ASSERT_EQUAL_INT(J1939_CLAIM_CLAIMING, s_claim.state);
    TEST_ASSERT_EQUAL_HEX8(0xFA, s_claim.sa);
    TEST_ASSERT_EQUAL_UINT32(1, s_claim.stats.lost);

    /* the wait starts over from the move */
    j1939_claim_tick(&s_claim, MS(300));
    TEST_ASSERT_FALSE(j1939_claim_ready(&s_claim));
    j1939_claim_tick(&s_claim, MS(350));
    TEST_ASSERT_TRUE(j1939_claim_ready(&s_claim));

    /* lost again, once claimed: the dynamic range, from 128 */
    j1939_claim_rx(&s_claim, 0xFA, zeros, MS(400), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(0x80, out.sa);
    TEST_ASSERT_EQUAL_INT(J1939_CLAIM_CLAIMING, s_claim.state);
    j1939_claim_rx(&s_claim, 0x80, zeros, MS(450), &out);
    TEST_ASSERT_EQUAL_HEX8(0x81, out.sa);
    TEST_ASSERT_EQUAL_UINT32(3, s_claim.stats.lost);
    TEST_ASSERT_EQUAL_UINT32(4, s_claim.stats.claims_sent);
}

void test_claim_out_of_addresses_says_cannot_claim(void)
{
    j1939_claim_out_t out;
    uint8_t zeros[8];
    unsigned moves = 0;

    memset(zeros, 0x00, 8);
    claim_fresh();
    j1939_claim_start(&s_claim, 0, &out);

    /* a bus where every address is taken by a better NAME */
    while (s_claim.state == J1939_CLAIM_CLAIMING && moves < 300)
    {
        j1939_claim_rx(&s_claim, s_claim.sa, zeros, MS(moves), &out);
        moves++;
    }

    /* tool 1 (preferred), tool 2, then 128..247: 122 claims lost */
    TEST_ASSERT_EQUAL_INT(J1939_CLAIM_CANNOT, s_claim.state);
    TEST_ASSERT_EQUAL_UINT32(122, s_claim.stats.lost);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_NULL, out.sa); /* cannot claim: from 254 */
    TEST_ASSERT_EQUAL_MEMORY(s_name, out.data, 8);
    TEST_ASSERT_EQUAL_UINT32(1, s_claim.stats.cannot);
    TEST_ASSERT_FALSE(j1939_claim_ready(&s_claim));
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_NULL, s_claim.sa);

    /* nothing more to contest */
    j1939_claim_rx(&s_claim, 0x80, zeros, MS(999), &out);
    TEST_ASSERT_FALSE(out.send);

    /* a request for address claimed gets the cannot-claim again */
    j1939_claim_request(&s_claim, MS(1000), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_NULL, out.sa);
    TEST_ASSERT_EQUAL_UINT32(2, s_claim.stats.cannot);
    TEST_ASSERT_EQUAL_STRING("cannot_claim",
                             j1939_claim_state_name(s_claim.state));
}

void test_claim_request_for_address_claimed(void)
{
    j1939_claim_out_t out;

    claim_fresh();

    /* idle: nothing to say */
    j1939_claim_request(&s_claim, 0, &out);
    TEST_ASSERT_FALSE(out.send);

    j1939_claim_start(&s_claim, 0, &out);
    j1939_claim_request(&s_claim, MS(100), &out); /* while still claiming */
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_HEX8(0xF9, out.sa);
    TEST_ASSERT_EQUAL_INT(J1939_CLAIM_CLAIMING, s_claim.state);

    /* the wait is not restarted by a request */
    j1939_claim_tick(&s_claim, MS(250));
    TEST_ASSERT_TRUE(j1939_claim_ready(&s_claim));

    j1939_claim_request(&s_claim, MS(300), &out);
    TEST_ASSERT_TRUE(out.send);
    TEST_ASSERT_EQUAL_MEMORY(s_name, out.data, 8);
    TEST_ASSERT_EQUAL_UINT32(2, s_claim.stats.requests);
    TEST_ASSERT_EQUAL_UINT32(3, s_claim.stats.claims_sent);
}

/* ---- request and acknowledgment --------------------------------------------------- */

void test_request_and_ackm_codec(void)
{
    uint8_t d[8];
    uint32_t pgn = 0;
    j1939_ackm_t a;

    /* j1939_ref.request(0xFEEC) = EC FE 00 */
    j1939_request_build(J1939_PGN_VIN, d);
    TEST_ASSERT_EQUAL_HEX8(0xEC, d[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, d[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, d[2]);
    TEST_ASSERT_TRUE(j1939_request_parse(d, 3, &pgn));
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_VIN, pgn);
    TEST_ASSERT_FALSE(j1939_request_parse(d, 2, &pgn));
    TEST_ASSERT_FALSE(j1939_request_parse(NULL, 3, &pgn));

    /* a request padded to 8 bytes is still a request */
    memset(d, 0xFF, 8);
    j1939_request_build(J1939_PGN_DM11, d);
    TEST_ASSERT_TRUE(j1939_request_parse(d, 8, &pgn));
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_DM11, pgn);

    /* j1939_ref.ackm(NACK, 0xFEEC, addr=0xF9) = 01 FF FF FF F9 EC FE 00 */
    j1939_ackm_build(J1939_ACK_NEGATIVE, 0xF9, J1939_PGN_VIN, d);
    TEST_ASSERT_EQUAL_HEX8(0x01, d[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, d[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, d[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, d[3]);
    TEST_ASSERT_EQUAL_HEX8(0xF9, d[4]);
    TEST_ASSERT_EQUAL_HEX8(0xEC, d[5]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, d[6]);
    TEST_ASSERT_EQUAL_HEX8(0x00, d[7]);

    TEST_ASSERT_TRUE(j1939_ackm_parse(d, 8, &a));
    TEST_ASSERT_EQUAL_UINT8(J1939_ACK_NEGATIVE, a.control);
    TEST_ASSERT_EQUAL_HEX8(0xFF, a.group_function);
    TEST_ASSERT_EQUAL_HEX8(0xF9, a.address);
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_VIN, a.pgn);
    TEST_ASSERT_FALSE(j1939_ackm_parse(d, 7, &a));

    TEST_ASSERT_EQUAL_STRING("ack", j1939_ack_name(J1939_ACK_POSITIVE));
    TEST_ASSERT_EQUAL_STRING("nack", j1939_ack_name(J1939_ACK_NEGATIVE));
    TEST_ASSERT_EQUAL_STRING("denied", j1939_ack_name(J1939_ACK_DENIED));
    TEST_ASSERT_EQUAL_STRING("busy", j1939_ack_name(J1939_ACK_BUSY));
    TEST_ASSERT_EQUAL_STRING("?", j1939_ack_name(9));
}

/* ---- transport protocol, this node the destination -------------------------------- */

/* DM1 with three trouble codes, 14 bytes, 2 packets, engine -> F9 */
static const uint8_t DM1_RTS[8] = { 0x10, 0x0E, 0x00, 0x02, 0xFF, 0xCA, 0xFE, 0x00 };
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

/* the VIN, 18 bytes, 3 packets, with the sender taking 1 packet per CTS */
static const uint8_t VIN_RTS_1[8] = { 0x10, 0x12, 0x00, 0x03, 0x01, 0xEC, 0xFE, 0x00 };
static const uint8_t VIN_DT[3][8] =
{
    { 0x01, 0x31, 0x57, 0x43, 0x41, 0x4E, 0x4A, 0x31 },
    { 0x02, 0x39, 0x33, 0x39, 0x54, 0x52, 0x55, 0x43 },
    { 0x03, 0x4B, 0x30, 0x31, 0x2A, 0xFF, 0xFF, 0xFF },
};

static void books_balance(void)
{
    const j1939_tp_stats_t *st = &s_tp.stats;

    TEST_ASSERT_EQUAL_UINT32(st->started,
                             st->completed + st->seq_errors + st->timeouts +
                                 st->aborted + st->replaced +
                                 (uint32_t)j1939_tp_open(&s_tp));
}

static void expect_reply(uint8_t da, const uint8_t data[8])
{
    j1939_tp_reply_t r;

    TEST_ASSERT_TRUE(j1939_tp_reply_take(&s_tp, &r));
    TEST_ASSERT_EQUAL_HEX8(da, r.da);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, r.data, 8);
}

void test_tp_dest_rts_cts_eoma(void)
{
    j1939_tp_msg_t m = { 0 };
    const uint8_t cts[8]  = { 0x11, 0x02, 0x01, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
    const uint8_t eoma[8] = { 0x13, 0x0E, 0x00, 0x02, 0xFF, 0xCA, 0xFE, 0x00 };

    j1939_tp_init(&s_tp);
    TEST_ASSERT_EQUAL_HEX8(J1939_ADDR_NULL, s_tp.my_sa);
    j1939_tp_set_address(&s_tp, 0xF9);

    /* the engine answers our request for DM1 with a request to send */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(0));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.to_me);
    expect_reply(0x00, cts); /* both packets at once, from packet 1 */
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));

    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(10), &m));
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL)); /* window not over */
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[1], 8, MS(20), &m));
    TEST_ASSERT_EQUAL_HEX32(J1939_PGN_DM1, m.pgn);
    TEST_ASSERT_EQUAL_HEX8(0xF9, m.da);
    TEST_ASSERT_EQUAL_UINT16(14, m.len);
    TEST_ASSERT_EQUAL_MEMORY(DM1_MSG, m.data, 14);
    expect_reply(0x00, eoma);
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));

    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.cts);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.eoma);
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.aborts_out);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.completed);
    books_balance();
}

void test_tp_dest_window_of_one_packet(void)
{
    j1939_tp_msg_t m = { 0 };
    const uint8_t cts1[8] = { 0x11, 0x01, 0x01, 0xFF, 0xFF, 0xEC, 0xFE, 0x00 };
    const uint8_t cts2[8] = { 0x11, 0x01, 0x02, 0xFF, 0xFF, 0xEC, 0xFE, 0x00 };
    const uint8_t cts3[8] = { 0x11, 0x01, 0x03, 0xFF, 0xFF, 0xEC, 0xFE, 0x00 };
    const uint8_t eoma[8] = { 0x13, 0x12, 0x00, 0x03, 0xFF, 0xEC, 0xFE, 0x00 };

    j1939_tp_init(&s_tp);
    j1939_tp_set_address(&s_tp, 0xF9);

    /* RTS byte 4 = 1: the sender wants a clear-to-send per packet */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, VIN_RTS_1, 8, MS(0));
    expect_reply(0x00, cts1);
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, VIN_DT[0], 8, MS(10), &m));
    expect_reply(0x00, cts2);
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, VIN_DT[1], 8, MS(20), &m));
    expect_reply(0x00, cts3);
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xF9, VIN_DT[2], 8, MS(30), &m));
    expect_reply(0x00, eoma);
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));
    TEST_ASSERT_EQUAL_MEMORY("1WCANJ1939TRUCK01*", m.data, 18);
    TEST_ASSERT_EQUAL_UINT32(3, s_tp.stats.cts);
    books_balance();
}

void test_tp_dest_aborts(void)
{
    j1939_tp_msg_t m = { 0 };
    const uint8_t abort_seq[8]  = { 0xFF, 0x07, 0xFF, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
    const uint8_t abort_time[8] = { 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
    const uint8_t abort_busy[8] = { 0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xCA, 0xFE, 0x00 };
    uint8_t rts[8];

    j1939_tp_init(&s_tp);
    j1939_tp_set_address(&s_tp, 0xF9);

    /* a packet out of sequence: the connection is aborted (reason 7) */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(0));
    TEST_ASSERT_TRUE(j1939_tp_reply_take(&s_tp, NULL)); /* the CTS */
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[1], 8, MS(10), &m));
    expect_reply(0x00, abort_seq);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.seq_errors);
    TEST_ASSERT_EQUAL_UINT32(0, j1939_tp_open(&s_tp));

    /* the sender goes quiet after our clear-to-send (reason 3) */
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(1000));
    TEST_ASSERT_TRUE(j1939_tp_reply_take(&s_tp, NULL));
    j1939_tp_expire(&s_tp, MS(2000));
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL)); /* within T2 */
    j1939_tp_expire(&s_tp, MS(2300));
    expect_reply(0x00, abort_time);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.timeouts);

    /* every session taken: a request to us is refused, not left hanging */
    memcpy(rts, DM1_RTS, 8);

    for (unsigned i = 0; i < J1939_TP_SESSIONS; i++)
    {
        j1939_tp_cm(&s_tp, (uint8_t)(0x10 + i), 0xF9, rts, 8, MS(3000));
        TEST_ASSERT_TRUE(j1939_tp_reply_take(&s_tp, NULL));
    }

    j1939_tp_cm(&s_tp, 0x00, 0xF9, rts, 8, MS(3001));
    expect_reply(0x00, abort_busy);
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.no_session);
    TEST_ASSERT_EQUAL_UINT32(3, s_tp.stats.aborts_out);
    books_balance();
}

void test_tp_dest_only_with_an_address(void)
{
    j1939_tp_msg_t m = { 0 };

    /* a listener (no address) follows a connection to F9 but never answers */
    j1939_tp_init(&s_tp);
    j1939_tp_cm(&s_tp, 0x00, 0xF9, DM1_RTS, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.to_me);
    TEST_ASSERT_FALSE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[0], 8, MS(10), &m));
    TEST_ASSERT_TRUE(j1939_tp_dt(&s_tp, 0x00, 0xF9, DM1_DT[1], 8, MS(20), &m));
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));

    /* with an address, a connection to somebody ELSE is still only read */
    j1939_tp_init(&s_tp);
    j1939_tp_set_address(&s_tp, 0xF9);
    j1939_tp_cm(&s_tp, 0x00, 0x11, DM1_RTS, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, s_tp.stats.to_me);

    /* and a BAM to everyone gets no reply either */
    const uint8_t bam[8] = { 0x20, 0x12, 0x00, 0x03, 0xFF, 0xEC, 0xFE, 0x00 };

    j1939_tp_cm(&s_tp, 0x00, 0xFF, bam, 8, MS(0));
    TEST_ASSERT_FALSE(j1939_tp_reply_take(&s_tp, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, j1939_tp_open(&s_tp));
    books_balance();
}

void test_tp_dest_reply_queue_bound(void)
{
    uint8_t rts[8];

    j1939_tp_init(&s_tp);
    j1939_tp_set_address(&s_tp, 0xF9);
    memcpy(rts, DM1_RTS, 8);

    /* eight senders at once fill the sessions AND the reply queue; a ninth
       request's abort has no room and is counted, not lost quietly */
    for (unsigned i = 0; i < J1939_TP_SESSIONS; i++)
    {
        j1939_tp_cm(&s_tp, (uint8_t)(0x10 + i), 0xF9, rts, 8, MS(0));
    }

    TEST_ASSERT_EQUAL_UINT8(J1939_TP_REPLIES, s_tp.reply_n);
    j1939_tp_cm(&s_tp, 0x00, 0xF9, rts, 8, MS(1));
    TEST_ASSERT_EQUAL_UINT32(1, s_tp.stats.reply_lost);

    unsigned taken = 0;

    while (j1939_tp_reply_take(&s_tp, NULL))
    {
        taken++;
    }

    TEST_ASSERT_EQUAL_UINT(J1939_TP_REPLIES, taken);
}

void run_active_tests(void)
{
    RUN_TEST(test_name_build_matches_the_reference);
    RUN_TEST(test_name_compare_lower_wins);
    RUN_TEST(test_claim_preferred_address_after_the_wait);
    RUN_TEST(test_claim_contest_won_defends_without_a_new_wait);
    RUN_TEST(test_claim_defence_once_per_round);
    RUN_TEST(test_claim_contest_lost_moves_to_the_next_address);
    RUN_TEST(test_claim_out_of_addresses_says_cannot_claim);
    RUN_TEST(test_claim_request_for_address_claimed);
    RUN_TEST(test_request_and_ackm_codec);
    RUN_TEST(test_tp_dest_rts_cts_eoma);
    RUN_TEST(test_tp_dest_window_of_one_packet);
    RUN_TEST(test_tp_dest_aborts);
    RUN_TEST(test_tp_dest_only_with_an_address);
    RUN_TEST(test_tp_dest_reply_queue_bound);
}
