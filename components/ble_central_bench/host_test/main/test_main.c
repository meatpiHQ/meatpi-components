/**
 * @file test_main.c
 * @brief Host suite for ble_central_bench's pure tunnel client: header
 *        round trip and rejects, request head JSON, response head parse,
 *        the feed state machine (frames split at arbitrary boundaries,
 *        credits, response + body + LAST, stray sequence numbers, resync
 *        after garbage) and the credit window rule.
 */
#include <string.h>

#include "unity.h"

#include "ble_central_bench_tunnel.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- header ------------------------------------------------------------------- */

static void test_hdr_roundtrip(void)
{
    uint8_t b[BCBT_HDR];
    uint8_t t, f;
    uint16_t s, l;

    bcbt_hdr_build(b, BCBT_T_REQ_BODY, BCBT_F_LAST, 0x1234, 4096);
    TEST_ASSERT_EQUAL_HEX8(0x57, b[0]);
    TEST_ASSERT_EQUAL_HEX8(BCBT_VERSION, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, b[1]); /* protocol v2 */
    TEST_ASSERT_TRUE(bcbt_hdr_parse(b, &t, &f, &s, &l));
    TEST_ASSERT_EQUAL(BCBT_T_REQ_BODY, t);
    TEST_ASSERT_EQUAL(BCBT_F_LAST, f);
    TEST_ASSERT_EQUAL_HEX16(0x1234, s);
    TEST_ASSERT_EQUAL(4096, l);
}

static void test_hdr_rejects(void)
{
    uint8_t b[BCBT_HDR];
    uint8_t t, f;
    uint16_t s, l;

    bcbt_hdr_build(b, BCBT_T_RSP, 0, 1, 10);
    b[0] = 0x58;                                    /* magic */
    TEST_ASSERT_FALSE(bcbt_hdr_parse(b, &t, &f, &s, &l));
    bcbt_hdr_build(b, 7, 0, 1, 10);                 /* type  */
    TEST_ASSERT_FALSE(bcbt_hdr_parse(b, &t, &f, &s, &l));
    bcbt_hdr_build(b, BCBT_T_RSP, 0, 1, 10);
    b[1] = 1;                                       /* a v1 device */
    TEST_ASSERT_FALSE(bcbt_hdr_parse(b, &t, &f, &s, &l));
    bcbt_hdr_build(b, BCBT_T_RSP, 0, 1, 4097);      /* len   */
    TEST_ASSERT_FALSE(bcbt_hdr_parse(b, &t, &f, &s, &l));
}

/* ---- heads -------------------------------------------------------------------- */

static void test_req_head(void)
{
    char h[BCBT_HEAD_MAX];
    size_t n = bcbt_req_head(h, sizeof(h), "POST", "/api/fs/upload?path=/data/x.bin",
                             "application/octet-stream", 65536);

    TEST_ASSERT_EQUAL_STRING(
        "{\"m\":\"POST\",\"p\":\"/api/fs/upload?path=/data/x.bin\","
        "\"ct\":\"application/octet-stream\",\"len\":65536}", h);
    TEST_ASSERT_EQUAL(strlen(h), n);

    n = bcbt_req_head(h, sizeof(h), "GET", "/api/status", NULL, 0);
    TEST_ASSERT_EQUAL_STRING("{\"m\":\"GET\",\"p\":\"/api/status\",\"len\":0}", h);

    TEST_ASSERT_EQUAL(0, bcbt_req_head(h, 20, "GET", "/api/status", NULL, 0)); /* no fit */
}

static void test_rsp_head_parse(void)
{
    int st;
    int64_t len;
    const char *a = "{\"s\":200,\"ct\":\"application/json\",\"len\":1234}";
    const char *b = "{\"s\":404,\"err\":\"path\"}";
    const char *c = "{\"s\":200,\"len\":-1}";

    TEST_ASSERT_TRUE(bcbt_rsp_head_parse(a, strlen(a), &st, &len));
    TEST_ASSERT_EQUAL(200, st);
    TEST_ASSERT_EQUAL(1234, len);
    TEST_ASSERT_TRUE(bcbt_rsp_head_parse(b, strlen(b), &st, &len));
    TEST_ASSERT_EQUAL(404, st);
    TEST_ASSERT_EQUAL(-1, len);
    TEST_ASSERT_TRUE(bcbt_rsp_head_parse(c, strlen(c), &st, &len));
    TEST_ASSERT_EQUAL(-1, len);
    TEST_ASSERT_FALSE(bcbt_rsp_head_parse("{\"x\":1}", 7, &st, &len));
}

/* ---- the feed state machine ---------------------------------------------------- */

static struct
{
    int credits, rsps, bodies, dones, strays, holes;
    uint32_t last_credit, last_status, last_hole;
    uint8_t body[9000];
    size_t body_len;
} E;

static void on_event(void *arg, bcbt_event_t ev, const uint8_t *p, size_t n, uint32_t v)
{
    (void)arg;

    switch (ev)
    {
        case BCBT_EV_CREDIT: E.credits++; E.last_credit = v; break;
        case BCBT_EV_RSP:    E.rsps++; E.last_status = v; break;
        case BCBT_EV_BODY:   E.bodies++; memcpy(E.body + E.body_len, p, n); E.body_len += n; break;
        case BCBT_EV_DONE:   E.dones++; break;
        case BCBT_EV_STRAY:  E.strays++; break;
        case BCBT_EV_HOLE:   E.holes++; E.last_hole = v; break;
        default: break;
    }
}

static size_t mk(uint8_t *dst, uint8_t type, uint8_t flags, uint16_t seq,
                 const uint8_t *pl, uint16_t len)
{
    bcbt_hdr_build(dst, type, flags, seq, len);

    if (len)
    {
        memcpy(dst + BCBT_HDR, pl, len);
    }

    return BCBT_HDR + len;
}

static void test_feed_response_split_bytes(void)
{
    static uint8_t wire[12000];
    static uint8_t body[5000];
    bcbt_client_t c;
    size_t n = 0;
    const char *head = "{\"s\":200,\"ct\":\"application/octet-stream\",\"len\":5000}";
    uint8_t cr[4] = { 0x00, 0x20, 0x00, 0x00 }; /* 8192 */

    for (size_t i = 0; i < sizeof(body); i++)
    {
        body[i] = (uint8_t)(i * 7);
    }

    memset(&E, 0, sizeof(E));
    bcbt_client_begin(&c, 42);
    n += mk(wire + n, BCBT_T_CREDIT, 0, 42, cr, 4);
    n += mk(wire + n, BCBT_T_RSP, 0, 42, (const uint8_t *)head, (uint16_t)strlen(head));
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(0, false), 42, body, 4096);
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(1, true), 42, body + 4096, 904);

    /* feed in odd slices: 1, 7, 8, 490, 3 ... like notifications would */
    size_t sizes[] = { 1, 7, 8, 490, 3, 1000, 490, 490, 490, 490, 4000 };
    size_t off = 0, k = 0;

    while (off < n)
    {
        size_t s = sizes[k++ % (sizeof(sizes) / sizeof(sizes[0]))];

        if (s > n - off)
        {
            s = n - off;
        }

        bcbt_client_feed(&c, wire + off, s, on_event, NULL);
        off += s;
    }

    TEST_ASSERT_EQUAL(1, E.credits);
    TEST_ASSERT_EQUAL(8192, E.last_credit);
    TEST_ASSERT_EQUAL(1, E.rsps);
    TEST_ASSERT_EQUAL(200, E.last_status);
    TEST_ASSERT_EQUAL(2, E.bodies);
    TEST_ASSERT_EQUAL(1, E.dones);
    TEST_ASSERT_EQUAL(5000, E.body_len);
    TEST_ASSERT_EQUAL_MEMORY(body, E.body, 5000);
    TEST_ASSERT_TRUE(c.done);
    TEST_ASSERT_EQUAL(200, c.status);
    TEST_ASSERT_EQUAL(5000, c.rsp_len);
    TEST_ASSERT_EQUAL(0, c.resync);
    TEST_ASSERT_EQUAL(0, E.strays);
    TEST_ASSERT_EQUAL(0, E.holes);
    TEST_ASSERT_EQUAL(0, c.holes);
    TEST_ASSERT_EQUAL(5000, c.body_rx);
}

static void test_feed_stray_and_resync(void)
{
    uint8_t wire[600];
    bcbt_client_t c;
    size_t n = 0;
    const char *head = "{\"s\":204,\"len\":0}";

    memset(&E, 0, sizeof(E));
    bcbt_client_begin(&c, 7);
    wire[n++] = 0xAA;                                /* garbage byte first   */
    wire[n++] = 0xBB;
    n += mk(wire + n, BCBT_T_RSP, 0, 8, (const uint8_t *)head, (uint16_t)strlen(head)); /* other seq */
    n += mk(wire + n, BCBT_T_RSP, 0, 7, (const uint8_t *)head, (uint16_t)strlen(head));
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(0, true), 7, NULL, 0); /* empty LAST */

    bcbt_client_feed(&c, wire, n, on_event, NULL);

    TEST_ASSERT_EQUAL(2, c.resync);
    TEST_ASSERT_EQUAL(1, E.strays);
    TEST_ASSERT_EQUAL(1, E.rsps);
    TEST_ASSERT_EQUAL(204, E.last_status);
    TEST_ASSERT_EQUAL(0, E.bodies);                  /* empty body: no BODY event */
    TEST_ASSERT_EQUAL(1, E.dones);
}

static void test_credit_window(void)
{
    bcbt_client_t c;

    bcbt_client_begin(&c, 1);
    TEST_ASSERT_TRUE(bcbt_client_may_send(&c, 0));
    TEST_ASSERT_TRUE(bcbt_client_may_send(&c, BCBT_CREDIT_WIN - 1));
    TEST_ASSERT_FALSE(bcbt_client_may_send(&c, BCBT_CREDIT_WIN));
    c.credited = 8192;
    TEST_ASSERT_TRUE(bcbt_client_may_send(&c, 8192 + BCBT_CREDIT_WIN - 1));
    TEST_ASSERT_FALSE(bcbt_client_may_send(&c, 8192 + BCBT_CREDIT_WIN));
}

/* ---- v2: the body-frame counter and the credits we owe ------------------------ */

static void test_body_counter_holes(void)
{
    static uint8_t wire[6000];
    uint8_t pl[482];
    bcbt_client_t c;
    size_t n = 0;
    const char *head = "{\"s\":200,\"len\":-1}";

    memset(pl, 0x5A, sizeof(pl));
    memset(&E, 0, sizeof(E));
    bcbt_client_begin(&c, 3);
    n += mk(wire + n, BCBT_T_RSP, 0, 3, (const uint8_t *)head, (uint16_t)strlen(head));
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(0, false), 3, pl, 482);
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(1, false), 3, pl, 482);
    /* frame 2 lost (a dropped notification) */
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(3, false), 3, pl, 482);
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(4, false), 3, pl, 482);
    n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(5, true), 3, NULL, 0);

    bcbt_client_feed(&c, wire, n, on_event, NULL);

    TEST_ASSERT_EQUAL(1, E.holes);
    TEST_ASSERT_EQUAL(3, E.last_hole);          /* the counter that arrived */
    TEST_ASSERT_EQUAL(1, c.holes);
    TEST_ASSERT_EQUAL(4, E.bodies);             /* the frames that did arrive */
    TEST_ASSERT_EQUAL(4 * 482, c.body_rx);
    TEST_ASSERT_TRUE(c.done);

    /* the counter wraps 15 -> 0 without a hole */
    memset(&E, 0, sizeof(E));
    bcbt_client_begin(&c, 4);
    n = 0;

    for (int i = 0; i < 18; i++)
    {
        n += mk(wire + n, BCBT_T_RSP_BODY, BCBT_CTR_FLAGS(i, i == 17), 4, pl, 100);
    }

    bcbt_client_feed(&c, wire, n, on_event, NULL);
    TEST_ASSERT_EQUAL(0, c.holes);
    TEST_ASSERT_EQUAL(18, E.bodies);
    TEST_ASSERT_EQUAL(1800, c.body_rx);
}

static void test_credit_due_and_frame(void)
{
    bcbt_client_t c;
    uint8_t f[BCBT_HDR + 4];
    uint8_t t, fl;
    uint16_t s, l;

    bcbt_client_begin(&c, 0x1234);
    TEST_ASSERT_FALSE(bcbt_client_credit_due(&c, 4096));
    c.body_rx = 4095;
    TEST_ASSERT_FALSE(bcbt_client_credit_due(&c, 4096));
    c.body_rx = 4096;
    TEST_ASSERT_TRUE(bcbt_client_credit_due(&c, 4096));

    TEST_ASSERT_EQUAL(BCBT_HDR + 4, bcbt_client_credit_frame(&c, f));
    TEST_ASSERT_TRUE(bcbt_hdr_parse(f, &t, &fl, &s, &l));
    TEST_ASSERT_EQUAL(BCBT_T_CREDIT, t);
    TEST_ASSERT_EQUAL_HEX16(0x1234, s);
    TEST_ASSERT_EQUAL(4, l);
    TEST_ASSERT_EQUAL_HEX8(0x00, f[BCBT_HDR + 0]);   /* 4096 LE */
    TEST_ASSERT_EQUAL_HEX8(0x10, f[BCBT_HDR + 1]);
    TEST_ASSERT_EQUAL(4096, c.credit_sent);
    TEST_ASSERT_FALSE(bcbt_client_credit_due(&c, 4096)); /* paid */

    c.body_rx = 9000;
    TEST_ASSERT_TRUE(bcbt_client_credit_due(&c, 4096));
    c.done = true;                                        /* nothing owed after LAST */
    TEST_ASSERT_FALSE(bcbt_client_credit_due(&c, 4096));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hdr_roundtrip);
    RUN_TEST(test_hdr_rejects);
    RUN_TEST(test_req_head);
    RUN_TEST(test_rsp_head_parse);
    RUN_TEST(test_feed_response_split_bytes);
    RUN_TEST(test_feed_stray_and_resync);
    RUN_TEST(test_credit_window);
    RUN_TEST(test_body_counter_holes);
    RUN_TEST(test_credit_due_and_frame);
    UNITY_END();
}
