/**
 * @file test_main.c
 * @brief Host suite for ble_http's pure core: frame codec (round trip,
 *        bounds, resync, splits), REQ head parsing (methods, the /api/
 *        prefix, bad JSON, len bounds), RSP head building, and the tunnel
 *        state machine against a fake HTTP side (body forwarding, CREDIT
 *        cadence, LAST/short/overrun, ABORT, idle timeout, busy, link
 *        down, chunked responses, upstream failures).
 */
#include <string.h>

#include "unity.h"

#include "ble_http_core.h"

/* ---- fake HTTP side ------------------------------------------------------------- */

static struct
{
    int   open_calls, write_calls, fetch_calls, read_calls, close_calls;
    bleh_req_t last_req;
    uint8_t body[70000];
    size_t body_len;
    esp_err_t open_rc, write_rc, fetch_rc;
    int status;
    const char *ct;
    int64_t len;
    const uint8_t *rsp;
    size_t rsp_len, rsp_off, rsp_chunk;
    bool read_fail;
} F;

static esp_err_t f_open(void *ctx, const bleh_req_t *req)
{
    (void)ctx;
    F.open_calls++;
    F.last_req = *req;
    F.body_len = 0;
    return F.open_rc;
}

static esp_err_t f_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    F.write_calls++;

    if (F.body_len + n <= sizeof(F.body))
    {
        memcpy(F.body + F.body_len, d, n);
        F.body_len += n;
    }

    return F.write_rc;
}

static esp_err_t f_fetch(void *ctx, int *status, char *ct, size_t cap,
                         int64_t *len)
{
    (void)ctx;
    F.fetch_calls++;
    *status = F.status;
    strncpy(ct, F.ct != NULL ? F.ct : "", cap - 1);
    ct[cap - 1] = '\0';
    *len = F.len;
    F.rsp_off = 0;
    return F.fetch_rc;
}

static int f_read(void *ctx, uint8_t *dst, size_t cap)
{
    (void)ctx;
    F.read_calls++;

    if (F.read_fail)
    {
        return -1;
    }

    size_t left = F.rsp_len - F.rsp_off;
    size_t n = (left < cap) ? left : cap;

    if (F.rsp_chunk != 0 && n > F.rsp_chunk)
    {
        n = F.rsp_chunk;
    }

    memcpy(dst, F.rsp + F.rsp_off, n);
    F.rsp_off += n;
    return (int)n;
}

static void f_close(void *ctx)
{
    (void)ctx;
    F.close_calls++;
}

static const bleh_ops_t OPS = { f_open, f_write, f_fetch, f_read, f_close };

/* ---- capturing emit: every frame parsed into a list ------------------------------- */

typedef struct
{
    bleh_hdr_t hdr;
    uint8_t payload[BLEH_FRAME_MAX];
} cap_frame_t;

static cap_frame_t E[192];
static int E_n;
static bool E_fail;

static bool cap_emit(void *arg, const uint8_t *frame, size_t n)
{
    (void)arg;

    if (E_fail)
    {
        return false;
    }

    TEST_ASSERT_TRUE(n >= BLEH_HDR_SIZE);
    TEST_ASSERT_TRUE(E_n < 192);
    TEST_ASSERT_TRUE(bleh_hdr_parse(frame, &E[E_n].hdr));
    TEST_ASSERT_EQUAL(n - BLEH_HDR_SIZE, E[E_n].hdr.len);
    memcpy(E[E_n].payload, frame + BLEH_HDR_SIZE, E[E_n].hdr.len);
    E_n++;
    return true;
}

static bleh_core_t C;
static uint8_t RX[BLEH_FRAME_BUF];
static uint8_t TX[BLEH_FRAME_BUF];
static uint8_t W[BLEH_FRAME_BUF];

static void reset_all(void)
{
    memset(&F, 0, sizeof(F));
    F.status = 200;
    F.ct = "application/json";
    F.len = 0;
    E_n = 0;
    E_fail = false;
    bleh_core_init(&C, &OPS, NULL, cap_emit, NULL, RX, TX);
    bleh_core_set_link(&C, false, 490); /* indications: the v1 behaviour */
}

/* stream the whole active response (indication mode: never blocks on
   credits); a bounded loop so a stuck pump fails instead of hanging */
static void drain(uint32_t now)
{
    for (int i = 0; i < 200 && C.state == BLEH_ST_RESPONDING; i++)
    {
        if (!bleh_core_pump(&C, now) && C.state == BLEH_ST_RESPONDING)
        {
            break; /* blocked on the window */
        }
    }
}

static uint8_t BODY_CTR; /* the counter the fake app puts on REQ_BODY */

static void feed(uint8_t type, uint8_t flags, uint16_t seq, const void *p,
                 size_t n, uint32_t now)
{
    bleh_hdr_build(W, type, flags, seq, (uint16_t)n);

    if (n > 0)
    {
        memcpy(W + BLEH_HDR_SIZE, p, n);
    }

    bleh_core_rx(&C, W, BLEH_HDR_SIZE + n, now);
}

static void feed_req(uint16_t seq, const char *json, uint32_t now)
{
    BODY_CTR = 0;
    feed(BLEH_T_REQ, 0, seq, json, strlen(json), now);
}

/* a well-behaved app: REQ_BODY frames numbered 0, 1, 2, ... mod 16 */
static void feed_body(uint16_t seq, const void *p, size_t n, bool last,
                      uint32_t now)
{
    feed(BLEH_T_REQ_BODY, BLEH_CTR_FLAGS(BODY_CTR, last), seq, p, n, now);
    BODY_CTR = (uint8_t)((BODY_CTR + 1) & 0x0F);
}

static void feed_credit(uint16_t seq, uint32_t v, uint32_t now)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

    feed(BLEH_T_CREDIT, 0, seq, b, 4, now);
}

static int count_type(uint8_t type)
{
    int k = 0;

    for (int i = 0; i < E_n; i++)
    {
        if (E[i].hdr.type == type)
        {
            k++;
        }
    }

    return k;
}

static const char *rsp_json(int i)
{
    static char s[BLEH_FRAME_MAX + 1];

    memcpy(s, E[i].payload, E[i].hdr.len);
    s[E[i].hdr.len] = '\0';
    return s;
}

/* ---- codec --------------------------------------------------------------------------- */

static void test_hdr_roundtrip(void)
{
    uint8_t b[8];
    bleh_hdr_t h;

    bleh_hdr_build(b, BLEH_T_RSP_BODY, BLEH_F_LAST, 0xBEEF, 4096);
    TEST_ASSERT_EQUAL_HEX8(0x57, b[0]);
    TEST_ASSERT_EQUAL_HEX8(BLEH_VERSION, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, b[1]); /* v2 since 2026-09-22 */
    TEST_ASSERT_TRUE(bleh_hdr_parse(b, &h));
    TEST_ASSERT_EQUAL(BLEH_T_RSP_BODY, h.type);
    TEST_ASSERT_EQUAL(BLEH_F_LAST, h.flags);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, h.seq);
    TEST_ASSERT_EQUAL(4096, h.len);
}

static void test_hdr_rejects(void)
{
    uint8_t b[8];
    bleh_hdr_t h;

    bleh_hdr_build(b, BLEH_T_REQ, 0, 1, 0);
    b[0] = 0x58;
    TEST_ASSERT_FALSE(bleh_hdr_parse(b, &h));
    bleh_hdr_build(b, BLEH_T_REQ, 0, 1, 0);
    b[1] = 1; /* a v1 client is refused (no counter, no credits) */
    TEST_ASSERT_FALSE(bleh_hdr_parse(b, &h));
    b[1] = 3;
    TEST_ASSERT_FALSE(bleh_hdr_parse(b, &h));
    bleh_hdr_build(b, 0, 0, 1, 0);
    TEST_ASSERT_FALSE(bleh_hdr_parse(b, &h));
    bleh_hdr_build(b, 7, 0, 1, 0);
    TEST_ASSERT_FALSE(bleh_hdr_parse(b, &h));
    bleh_hdr_build(b, BLEH_T_REQ, 0, 1, 4097);
    TEST_ASSERT_FALSE(bleh_hdr_parse(b, &h));
}

static void test_resync_skips_garbage(void)
{
    reset_all();

    uint8_t junk[5] = { 1, 2, 3, 0x57, 9 };

    bleh_core_rx(&C, junk, sizeof(junk), 0);
    feed_req(7, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 0);
    TEST_ASSERT_EQUAL(1, F.open_calls);
    TEST_ASSERT_TRUE(C.ctr.resync >= 4);
}

static void test_split_feeds_reassemble(void)
{
    reset_all();

    const char *json = "{\"m\":\"GET\",\"p\":\"/api/info\"}";
    size_t n = strlen(json);

    bleh_hdr_build(W, BLEH_T_REQ, 0, 3, (uint16_t)n);
    memcpy(W + BLEH_HDR_SIZE, json, n);

    /* one byte at a time, then a 7-byte header split */
    for (size_t i = 0; i < BLEH_HDR_SIZE + n; i++)
    {
        bleh_core_rx(&C, W + i, 1, 0);
    }

    TEST_ASSERT_EQUAL(1, F.open_calls);
    TEST_ASSERT_EQUAL_STRING("/api/info", F.last_req.path);
    drain(0); /* v2: the response streams from the pump; finish it first */

    bleh_core_rx(&C, W, 7, 0);
    bleh_core_rx(&C, W + 7, BLEH_HDR_SIZE + n - 7, 0);
    TEST_ASSERT_EQUAL(2, F.open_calls);
}

/* ---- heads ---------------------------------------------------------------------------- */

static void test_req_parse_ok(void)
{
    bleh_req_t r;
    int st;
    const char *err;
    const char *j = "{\"m\":\"PUT\",\"p\":\"/api/settings/wifi_manager?x=1\","
                    "\"ct\":\"application/json\",\"len\":123}";

    TEST_ASSERT_TRUE(bleh_req_parse(j, strlen(j), &r, &st, &err));
    TEST_ASSERT_EQUAL_STRING("PUT", r.method);
    TEST_ASSERT_EQUAL_STRING("/api/settings/wifi_manager?x=1", r.path);
    TEST_ASSERT_EQUAL_STRING("application/json", r.ct);
    TEST_ASSERT_EQUAL_UINT32(123, r.len);
    TEST_ASSERT_NULL(err);
}

static void test_req_parse_rejects(void)
{
    bleh_req_t r;
    int st;
    const char *err;

    TEST_ASSERT_FALSE(bleh_req_parse("nope", 4, &r, &st, &err));
    TEST_ASSERT_EQUAL(400, st);
    TEST_ASSERT_EQUAL_STRING("bad_request", err);

    const char *m = "{\"m\":\"PATCH\",\"p\":\"/api/x\"}";
    TEST_ASSERT_FALSE(bleh_req_parse(m, strlen(m), &r, &st, &err));
    TEST_ASSERT_EQUAL_STRING("method", err);

    const char *p1 = "{\"m\":\"GET\",\"p\":\"/ui/index.html\"}";
    TEST_ASSERT_FALSE(bleh_req_parse(p1, strlen(p1), &r, &st, &err));
    TEST_ASSERT_EQUAL(403, st);
    TEST_ASSERT_EQUAL_STRING("path", err);

    const char *p2 = "{\"m\":\"GET\",\"p\":\"/api/fs/list?path=/data x\"}";
    TEST_ASSERT_FALSE(bleh_req_parse(p2, strlen(p2), &r, &st, &err));
    TEST_ASSERT_EQUAL(403, st);

    const char *p3 = "{\"m\":\"GET\",\"p\":\"/api/../ui\"}";
    TEST_ASSERT_FALSE(bleh_req_parse(p3, strlen(p3), &r, &st, &err));

    const char *l = "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":-1}";
    TEST_ASSERT_FALSE(bleh_req_parse(l, strlen(l), &r, &st, &err));
    TEST_ASSERT_EQUAL_STRING("len", err);

    const char *l2 = "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":9000000}";
    TEST_ASSERT_FALSE(bleh_req_parse(l2, strlen(l2), &r, &st, &err));
    TEST_ASSERT_EQUAL_STRING("len", err);
}

static void test_rsp_head(void)
{
    char s[96];

    TEST_ASSERT_TRUE(bleh_rsp_head(s, sizeof(s), 200, "text/plain", 12, NULL) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":200,\"ct\":\"text/plain\",\"len\":12}", s);
    bleh_rsp_head(s, sizeof(s), 200, NULL, -1, NULL);
    TEST_ASSERT_EQUAL_STRING("{\"s\":200,\"ct\":\"\",\"len\":-1}", s);
    bleh_rsp_head(s, sizeof(s), 429, NULL, 0, "busy");
    TEST_ASSERT_EQUAL_STRING("{\"s\":429,\"err\":\"busy\"}", s);
}

/* ---- FSM ------------------------------------------------------------------------------- */

static void test_get_streams_response(void)
{
    reset_all();
    static const uint8_t body[10000] = { 1 };
    F.rsp = body;
    F.rsp_len = sizeof(body);
    F.len = sizeof(body);

    feed_req(5, "{\"m\":\"GET\",\"p\":\"/api/fs/download?path=/data/a\"}", 100);

    TEST_ASSERT_EQUAL(1, F.open_calls);
    TEST_ASSERT_EQUAL(1, F.fetch_calls);
    /* v2: the head goes out at the REQ, the body from the pump */
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    TEST_ASSERT_EQUAL(1, E_n);
    drain(100);
    TEST_ASSERT_EQUAL(1, F.close_calls);
    /* RSP head, 3 body frames (4096, 4096, 1808), LAST (empty) */
    TEST_ASSERT_EQUAL(5, E_n);
    TEST_ASSERT_EQUAL(BLEH_T_RSP, E[0].hdr.type);
    TEST_ASSERT_EQUAL(5, E[0].hdr.seq);
    TEST_ASSERT_EQUAL_STRING("{\"s\":200,\"ct\":\"application/json\",\"len\":10000}",
                             rsp_json(0));
    TEST_ASSERT_EQUAL(BLEH_T_RSP_BODY, E[1].hdr.type);
    TEST_ASSERT_EQUAL(4096, E[1].hdr.len);
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(0, false), E[1].hdr.flags);
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(1, false), E[2].hdr.flags);
    TEST_ASSERT_EQUAL(1808, E[3].hdr.len);
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(2, false), E[3].hdr.flags);
    TEST_ASSERT_EQUAL(BLEH_T_RSP_BODY, E[4].hdr.type);
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(3, true), E[4].hdr.flags);
    TEST_ASSERT_EQUAL(0, E[4].hdr.len);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    TEST_ASSERT_EQUAL_UINT32(10000, C.ctr.bytes_out);
    TEST_ASSERT_EQUAL(200, C.ctr.last_status);
}

static void test_chunked_response_len_unknown(void)
{
    reset_all();
    static const uint8_t body[100] = { 2 };
    F.rsp = body;
    F.rsp_len = sizeof(body);
    F.len = -1;
    F.rsp_chunk = 30;

    feed_req(1, "{\"m\":\"GET\",\"p\":\"/api/logs/ring\"}", 0);
    drain(0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":200,\"ct\":\"application/json\",\"len\":-1}",
                             rsp_json(0));
    /* 30+30+30+10 then LAST */
    TEST_ASSERT_EQUAL(6, E_n);
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(4, true), E[5].hdr.flags);
}

static void test_upload_with_credits(void)
{
    reset_all();
    static uint8_t data[20000];

    for (size_t i = 0; i < sizeof(data); i++)
    {
        data[i] = (uint8_t)i;
    }

    feed_req(9, "{\"m\":\"POST\",\"p\":\"/api/fs/upload?path=/data/x\","
                "\"ct\":\"application/octet-stream\",\"len\":20000}", 0);
    TEST_ASSERT_EQUAL(BLEH_ST_BODY, C.state);
    TEST_ASSERT_EQUAL(0, E_n);

    size_t off = 0;

    while (off < sizeof(data))
    {
        size_t n = sizeof(data) - off;

        if (n > 4096)
        {
            n = 4096;
        }

        bool last = (off + n == sizeof(data));

        feed_body(9, data + off, n, last, 10);
        off += n;
    }

    TEST_ASSERT_EQUAL(sizeof(data), F.body_len);
    TEST_ASSERT_EQUAL_MEMORY(data, F.body, sizeof(data));
    TEST_ASSERT_EQUAL(1, F.fetch_calls);
    drain(10);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);

    /* 4096-byte frames: accepted 8192 and 16384 cross the 8 KB credit
       step (a CREDIT each), 20000 completes the body (RSP, no credit) */
    int credits = 0;

    for (int i = 0; i < E_n; i++)
    {
        if (E[i].hdr.type == BLEH_T_CREDIT)
        {
            uint32_t v = E[i].payload[0] | (E[i].payload[1] << 8) |
                         (E[i].payload[2] << 16) | ((uint32_t)E[i].payload[3] << 24);

            TEST_ASSERT_TRUE(v == 8192 || v == 16384);
            credits++;
        }
    }

    TEST_ASSERT_EQUAL(2, credits);
    TEST_ASSERT_EQUAL(BLEH_T_RSP, E[E_n - 2].hdr.type);
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(0, true), E[E_n - 1].hdr.flags);
}

static void test_upload_short_and_overrun(void)
{
    reset_all();
    uint8_t d[100] = { 0 };

    feed_req(2, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":200}", 0);
    feed_body(2, d, 100, true, 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":400,\"err\":\"short\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(1, F.close_calls);
    TEST_ASSERT_EQUAL(0, F.fetch_calls);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);

    reset_all();
    feed_req(3, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":50}", 0);
    feed_body(3, d, 100, false, 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":400,\"err\":\"len\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
}

static void test_busy_second_request(void)
{
    reset_all();
    feed_req(4, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":500}", 0);
    feed_req(5, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 0);
    TEST_ASSERT_EQUAL(BLEH_ST_BODY, C.state);
    TEST_ASSERT_EQUAL(1, F.open_calls);
    TEST_ASSERT_EQUAL(5, E[0].hdr.seq);
    TEST_ASSERT_EQUAL_STRING("{\"s\":429,\"err\":\"busy\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(BLEH_CTR_FLAGS(0, true), E[1].hdr.flags);
}

static void test_abort_and_stray_body(void)
{
    reset_all();
    uint8_t d[10] = { 0 };

    feed_req(6, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":500}", 0);
    feed(BLEH_T_ABORT, 0, 6, NULL, 0, 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":499,\"err\":\"aborted\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(1, F.close_calls);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);

    int before = E_n;

    feed_body(6, d, 10, false, 0); /* late body: ignored */
    TEST_ASSERT_EQUAL(before, E_n);
    TEST_ASSERT_EQUAL(0, F.write_calls);
}

static void test_idle_timeout(void)
{
    reset_all();
    feed_req(8, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":500}", 1000);
    bleh_core_tick(&C, 1000 + BLEH_IDLE_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(BLEH_ST_BODY, C.state);
    bleh_core_tick(&C, 1000 + BLEH_IDLE_TIMEOUT_MS + 1);
    TEST_ASSERT_EQUAL_STRING("{\"s\":504,\"err\":\"timeout\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    TEST_ASSERT_EQUAL_UINT32(1, C.ctr.timeouts);
}

static void test_link_down_mid_body(void)
{
    reset_all();
    uint8_t d[10] = { 0 };

    feed_req(8, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":500}", 0);
    bleh_core_rx(&C, d, 5, 0); /* a partial header in the reassembler */
    bleh_core_link_down(&C);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    TEST_ASSERT_EQUAL(1, F.close_calls);
    TEST_ASSERT_EQUAL(0, C.rx_len);
    TEST_ASSERT_EQUAL(0, E_n); /* nothing emitted into a dead link */
}

static void test_upstream_failures(void)
{
    reset_all();
    F.open_rc = ESP_FAIL;
    feed_req(1, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":502,\"err\":\"upstream\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(0, F.close_calls); /* never opened */

    reset_all();
    F.fetch_rc = ESP_FAIL;
    feed_req(2, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":502,\"err\":\"upstream\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(1, F.close_calls);

    reset_all();
    F.write_rc = ESP_FAIL;
    uint8_t d[10] = { 0 };
    feed_req(3, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":10}", 0);
    feed_body(3, d, 10, true, 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":502,\"err\":\"upstream\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
}

static void test_emit_failure_aborts(void)
{
    reset_all();
    static const uint8_t body[100] = { 0 };
    F.rsp = body;
    F.rsp_len = sizeof(body);
    E_fail = true;
    feed_req(1, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 0);
    TEST_ASSERT_EQUAL(1, F.close_calls);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
}

static void test_error_status_passthrough(void)
{
    reset_all();
    static const uint8_t body[] = "{\"error\":\"not found\"}";
    F.status = 404;
    F.rsp = body;
    F.rsp_len = sizeof(body) - 1;
    F.len = (int64_t)(sizeof(body) - 1);
    feed_req(1, "{\"m\":\"DELETE\",\"p\":\"/api/fs/file?path=/data/nope\"}", 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":404,\"ct\":\"application/json\",\"len\":21}",
                             rsp_json(0));
    TEST_ASSERT_EQUAL(404, C.ctr.last_status);
    TEST_ASSERT_EQUAL_UINT32(0, C.ctr.errors); /* an upstream 404 is not a tunnel error */
    drain(0);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
}

/* ---- v2: notify mode (PDU-aligned frames, app credits, counter) ------------------- */

static uint8_t BIG[40000];

static void big_init(void)
{
    for (size_t i = 0; i < sizeof(BIG); i++)
    {
        BIG[i] = (uint8_t)(i * 7 + (i >> 8));
    }
}

/* every RSP_BODY frame in E in order: sizes <= cap, counters 0,1,2.. mod 16,
   payload bytes equal to BIG[], exactly one LAST at the end */
static void assert_body_stream(size_t cap, size_t total)
{
    size_t off = 0;
    uint8_t ctr = 0;
    int last = 0;

    for (int i = 0; i < E_n; i++)
    {
        if (E[i].hdr.type != BLEH_T_RSP_BODY)
        {
            continue;
        }

        TEST_ASSERT_TRUE(E[i].hdr.len <= cap);
        TEST_ASSERT_EQUAL_HEX8(ctr, E[i].hdr.flags >> BLEH_F_CTR_SHIFT);
        ctr = (uint8_t)((ctr + 1) & 0x0F);

        if (E[i].hdr.len > 0)
        {
            TEST_ASSERT_EQUAL_MEMORY(BIG + off, E[i].payload, E[i].hdr.len);
            off += E[i].hdr.len;
        }

        if (E[i].hdr.flags & BLEH_F_LAST)
        {
            last++;
            TEST_ASSERT_EQUAL(0, E[i].hdr.len);
        }
    }

    TEST_ASSERT_EQUAL(total, off);
    TEST_ASSERT_EQUAL(1, last);
}

static void test_notify_mode_pdu_aligned_frames(void)
{
    reset_all();
    big_init();
    F.rsp = BIG;
    F.rsp_len = 10000;
    F.len = 10000;
    bleh_core_set_link(&C, true, 490); /* MTU 517: one frame per notification */

    feed_req(7, "{\"m\":\"GET\",\"p\":\"/api/fs/download?path=/data/a\"}", 0);
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    TEST_ASSERT_EQUAL(482, C.out_frame);
    TEST_ASSERT_EQUAL_UINT32(BLEH_OUT_WINDOW, C.out_window);

    /* 10000 B < the window: streams without any credit; 16 frames per pump */
    TEST_ASSERT_TRUE(bleh_core_pump(&C, 1));
    TEST_ASSERT_EQUAL(17, E_n); /* head + 16 */
    drain(2);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    /* 20 x 482 + 360 = 21 frames + LAST */
    TEST_ASSERT_EQUAL(1 + 21 + 1, E_n);
    assert_body_stream(482, 10000);
    TEST_ASSERT_EQUAL_UINT32(0, C.ctr.credit_stalls);
    TEST_ASSERT_EQUAL_UINT32(10000, C.ctr.bytes_out);
}

static void test_notify_window_waits_for_credit(void)
{
    reset_all();
    big_init();
    F.rsp = BIG;
    F.rsp_len = sizeof(BIG);
    F.len = sizeof(BIG);
    bleh_core_set_link(&C, true, 490);

    feed_req(8, "{\"m\":\"GET\",\"p\":\"/api/fs/download?path=/data/b\"}", 1000);
    drain(1000);
    /* blocked: 33 x 482 = 15906 in flight, one more would pass 16384 */
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    TEST_ASSERT_EQUAL_UINT32(15906, C.out_sent);
    TEST_ASSERT_EQUAL_UINT32(1, C.ctr.credit_stalls);
    int blocked_at = E_n;

    TEST_ASSERT_FALSE(bleh_core_pump(&C, 1001));
    TEST_ASSERT_EQUAL(blocked_at, E_n);

    /* a credit beyond what went out is ignored; a stale one too */
    feed_credit(8, 20000, 1002);
    TEST_ASSERT_EQUAL_UINT32(0, C.out_acked);
    feed_credit(8, 8192, 1003);
    TEST_ASSERT_EQUAL_UINT32(8192, C.out_acked);
    feed_credit(8, 4096, 1004);
    TEST_ASSERT_EQUAL_UINT32(8192, C.out_acked);
    TEST_ASSERT_EQUAL_UINT32(3, C.ctr.credits_rx);

    /* the pump resumes for exactly the credited room */
    drain(1005);
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    TEST_ASSERT_TRUE(C.out_sent - C.out_acked <= BLEH_OUT_WINDOW);
    TEST_ASSERT_TRUE(C.out_sent > 15906);

    /* the app keeps crediting everything it received: the stream completes */
    for (int i = 0; i < 40 && C.state == BLEH_ST_RESPONDING; i++)
    {
        feed_credit(8, C.out_sent, 1010 + (uint32_t)i);
        drain(1010 + (uint32_t)i);
    }

    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    assert_body_stream(482, sizeof(BIG));
    TEST_ASSERT_EQUAL_UINT32(sizeof(BIG), C.ctr.bytes_out);
    TEST_ASSERT_EQUAL(1, F.close_calls);
}

static void test_indicate_mode_needs_no_credit(void)
{
    reset_all();
    big_init();
    F.rsp = BIG;
    F.rsp_len = sizeof(BIG);
    F.len = sizeof(BIG);
    /* reset_all set indications, MTU 517 */
    feed_req(9, "{\"m\":\"GET\",\"p\":\"/api/fs/download?path=/data/c\"}", 0);
    TEST_ASSERT_EQUAL_UINT32(0, C.out_window);
    TEST_ASSERT_EQUAL(BLEH_FRAME_MAX, C.out_frame);
    drain(0);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    /* 9 x 4096 + 3136 = 10 frames + LAST */
    TEST_ASSERT_EQUAL(10 + 1, count_type(BLEH_T_RSP_BODY));
    assert_body_stream(BLEH_FRAME_MAX, sizeof(BIG));
    TEST_ASSERT_EQUAL_UINT32(0, C.ctr.credit_stalls);
}

static void test_req_body_hole_detected(void)
{
    reset_all();
    uint8_t d[100] = { 1 };

    feed_req(10, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":300}", 0);
    feed_body(10, d, 100, false, 0);           /* counter 0 */
    BODY_CTR = 2;                               /* frame 1 never arrived */
    feed_body(10, d, 100, false, 0);
    TEST_ASSERT_EQUAL_STRING("{\"s\":400,\"err\":\"hole\"}", rsp_json(0));
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    TEST_ASSERT_EQUAL_UINT32(1, C.ctr.holes);
    TEST_ASSERT_EQUAL(1, F.close_calls);
    TEST_ASSERT_EQUAL(100, F.body_len); /* the second frame was refused */

    /* the counter wraps at 16 */
    reset_all();
    feed_req(11, "{\"m\":\"POST\",\"p\":\"/api/fs/upload\",\"len\":1800}", 0);

    for (int i = 0; i < 18; i++)
    {
        feed_body(11, d, 100, i == 17, 0);
    }

    TEST_ASSERT_EQUAL(1800, F.body_len);
    TEST_ASSERT_EQUAL_UINT32(0, C.ctr.holes);
    TEST_ASSERT_EQUAL(1, F.fetch_calls);
}

static void test_credit_timeout_truncates(void)
{
    reset_all();
    big_init();
    F.rsp = BIG;
    F.rsp_len = sizeof(BIG);
    F.len = sizeof(BIG);
    bleh_core_set_link(&C, true, 490);

    feed_req(12, "{\"m\":\"GET\",\"p\":\"/api/fs/download?path=/data/d\"}", 5000);
    drain(5000);
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    int n = E_n;

    bleh_core_tick(&C, 5000 + BLEH_IDLE_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    bleh_core_tick(&C, 5000 + BLEH_IDLE_TIMEOUT_MS + 1);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    TEST_ASSERT_EQUAL(n + 1, E_n);
    TEST_ASSERT_EQUAL(BLEH_T_RSP_BODY, E[E_n - 1].hdr.type);
    TEST_ASSERT_TRUE(E[E_n - 1].hdr.flags & BLEH_F_LAST);
    TEST_ASSERT_EQUAL(0, E[E_n - 1].hdr.len);
    TEST_ASSERT_EQUAL_UINT32(1, C.ctr.timeouts);
    TEST_ASSERT_EQUAL(1, F.close_calls);
}

static void test_abort_and_busy_while_responding(void)
{
    reset_all();
    big_init();
    F.rsp = BIG;
    F.rsp_len = sizeof(BIG);
    F.len = sizeof(BIG);
    bleh_core_set_link(&C, true, 490);

    feed_req(13, "{\"m\":\"GET\",\"p\":\"/api/fs/download?path=/data/e\"}", 0);
    drain(0);
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);

    /* a second request while the download waits for credits: 429, the
       download stays open */
    int n = E_n;
    feed_req(14, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 1);
    TEST_ASSERT_EQUAL(n + 2, E_n);
    TEST_ASSERT_EQUAL(14, E[n].hdr.seq);
    TEST_ASSERT_EQUAL_STRING("{\"s\":429,\"err\":\"busy\"}", rsp_json(n));
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    TEST_ASSERT_EQUAL(1, F.open_calls);

    /* ABORT for another seq: ignored; for ours: LAST and idle */
    n = E_n;
    feed(BLEH_T_ABORT, 0, 99, NULL, 0, 2);
    TEST_ASSERT_EQUAL(BLEH_ST_RESPONDING, C.state);
    TEST_ASSERT_EQUAL(n, E_n);
    feed(BLEH_T_ABORT, 0, 13, NULL, 0, 3);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    TEST_ASSERT_EQUAL(n + 1, E_n);
    TEST_ASSERT_TRUE(E[E_n - 1].hdr.flags & BLEH_F_LAST);
    TEST_ASSERT_EQUAL(13, E[E_n - 1].hdr.seq);
    TEST_ASSERT_EQUAL(1, F.close_calls);
    TEST_ASSERT_EQUAL_UINT32(2, C.ctr.aborts);
}

static void test_small_mtu_notify_frames(void)
{
    reset_all();
    big_init();
    F.rsp = BIG;
    F.rsp_len = 100;
    F.len = 100;
    bleh_core_set_link(&C, true, 20); /* MTU 23: 12 B of payload per PDU */

    feed_req(15, "{\"m\":\"GET\",\"p\":\"/api/status\"}", 0);
    TEST_ASSERT_EQUAL(12, C.out_frame);
    drain(0);
    TEST_ASSERT_EQUAL(BLEH_ST_IDLE, C.state);
    assert_body_stream(12, 100); /* 8 x 12 + 4, then LAST: 10 frames */
    TEST_ASSERT_EQUAL(10, count_type(BLEH_T_RSP_BODY));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hdr_roundtrip);
    RUN_TEST(test_hdr_rejects);
    RUN_TEST(test_resync_skips_garbage);
    RUN_TEST(test_split_feeds_reassemble);
    RUN_TEST(test_req_parse_ok);
    RUN_TEST(test_req_parse_rejects);
    RUN_TEST(test_rsp_head);
    RUN_TEST(test_get_streams_response);
    RUN_TEST(test_chunked_response_len_unknown);
    RUN_TEST(test_upload_with_credits);
    RUN_TEST(test_upload_short_and_overrun);
    RUN_TEST(test_busy_second_request);
    RUN_TEST(test_abort_and_stray_body);
    RUN_TEST(test_idle_timeout);
    RUN_TEST(test_link_down_mid_body);
    RUN_TEST(test_upstream_failures);
    RUN_TEST(test_emit_failure_aborts);
    RUN_TEST(test_error_status_passthrough);
    RUN_TEST(test_notify_mode_pdu_aligned_frames);
    RUN_TEST(test_notify_window_waits_for_credit);
    RUN_TEST(test_indicate_mode_needs_no_credit);
    RUN_TEST(test_req_body_hole_detected);
    RUN_TEST(test_credit_timeout_truncates);
    RUN_TEST(test_abort_and_busy_while_responding);
    RUN_TEST(test_small_mtu_notify_frames);
    UNITY_END();
}
