/**
 * @file test_obd.c
 * @brief Host suite for the obd.* conversation core (script_engine_obd.c)
 *        over a fake port — SCRIPTING.md §4 item 5. Covers the claim
 *        state machine (claim/extend/busy/release/auto-release), the
 *        no-claim guards on request/isotp, and outcome plumbing.
 */
#include <string.h>

#include "unity.h"

#include "script_engine_obd.h"

/* ---- fake port -------------------------------------------------------------- */

static int f_begin_calls, f_end_calls, f_req_calls, f_tx_calls, f_rx_calls;
static int f_begin_rc, f_req_rc;
static se_obd_addr_t f_last_addr;
static se_obd_outcome_t f_req_outcome;
static uint8_t f_resp[8];
static size_t f_resp_len;

static int f_begin(const se_obd_addr_t *a)
{
    f_begin_calls++;
    f_last_addr = *a;
    return f_begin_rc;
}

static void f_end(void)
{
    f_end_calls++;
}

static int f_request(const se_obd_addr_t *a, const uint8_t *req,
                     size_t req_len, uint32_t timeout_ms, uint8_t *resp,
                     size_t resp_cap, size_t *resp_len,
                     se_obd_outcome_t *out)
{
    (void)req; (void)req_len; (void)timeout_ms;
    f_req_calls++;
    f_last_addr = *a;

    if (f_req_rc == 0)
    {
        size_t n = (f_resp_len <= resp_cap) ? f_resp_len : resp_cap;
        memcpy(resp, f_resp, n);
        *resp_len = n;

        if (out != NULL)
        {
            *out = f_req_outcome;
        }
    }

    return f_req_rc;
}

static int f_tx(const se_obd_addr_t *a, const uint8_t *d, size_t n,
                uint32_t t)
{
    (void)a; (void)d; (void)n; (void)t;
    f_tx_calls++;
    return 0;
}

static int f_rx(const se_obd_addr_t *a, uint8_t *o, size_t c, size_t *n,
                uint32_t t)
{
    (void)a; (void)t;
    f_rx_calls++;
    o[0] = 0x7E;
    (void)c;
    *n = 1;
    return 0;
}

static const se_obd_port_t FAKE =
{
    .session_begin = f_begin,
    .session_end   = f_end,
    .request       = f_request,
    .isotp_tx      = f_tx,
    .isotp_rx      = f_rx,
};

static void reset_fakes(void)
{
    f_begin_calls = f_end_calls = f_req_calls = f_tx_calls = f_rx_calls = 0;
    f_begin_rc = f_req_rc = 0;
    memset(&f_req_outcome, 0, sizeof(f_req_outcome));
    f_resp_len = 0;
    se_obd_set_port(&FAKE); /* also clears any claim */
}

static const se_obd_addr_t A1 = { .tx_id = 0x7E0, .rx_id = 0x7E8 };
static const se_obd_addr_t A2 = { .tx_id = 0x7E1, .rx_id = 0x7E9 };

/* ---- tests ------------------------------------------------------------------ */

void test_obd_claim_release_cycle(void)
{
    reset_fakes();

    TEST_ASSERT_FALSE(se_obd_claimed());
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));
    TEST_ASSERT_TRUE(se_obd_claimed());
    TEST_ASSERT_EQUAL_INT(1, f_begin_calls);
    TEST_ASSERT_EQUAL_UINT32(0x7E0, f_last_addr.tx_id);

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_release());
    TEST_ASSERT_FALSE(se_obd_claimed());
    TEST_ASSERT_EQUAL_INT(1, f_end_calls);
}

void test_obd_claim_same_addr_extends(void)
{
    reset_fakes();

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1)); /* idempotent */
    TEST_ASSERT_EQUAL_INT(1, f_begin_calls);             /* no re-begin */
}

void test_obd_claim_other_addr_busy(void)
{
    reset_fakes();

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_BUSY, se_obd_claim(&A2));
    TEST_ASSERT_TRUE(se_obd_claimed()); /* original claim intact */
}

void test_obd_claim_port_failure(void)
{
    reset_fakes();
    f_begin_rc = -1;

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_IO, se_obd_claim(&A1));
    TEST_ASSERT_FALSE(se_obd_claimed());
}

void test_obd_release_idempotent(void)
{
    reset_fakes();

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_release());
    TEST_ASSERT_EQUAL_INT(0, f_end_calls); /* nothing to end */
}

void test_obd_autorelease_after_run(void)
{
    reset_fakes();

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));
    se_obd_autorelease(); /* the runner's guaranteed hook */
    TEST_ASSERT_FALSE(se_obd_claimed());
    TEST_ASSERT_EQUAL_INT(1, f_end_calls);

    se_obd_autorelease(); /* second call = no-op */
    TEST_ASSERT_EQUAL_INT(1, f_end_calls);
}

void test_obd_request_requires_claim(void)
{
    reset_fakes();

    uint8_t req[] = { 0x22, 0xF1, 0x90 };
    uint8_t resp[16];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_NOCLAIM,
                          se_obd_request(req, sizeof(req), 0, resp,
                                         sizeof(resp), &n, NULL));
    TEST_ASSERT_EQUAL_INT(0, f_req_calls);
}

void test_obd_request_plumbs_outcome(void)
{
    reset_fakes();

    f_resp[0] = 0x62; f_resp[1] = 0xF1; f_resp[2] = 0x90;
    f_resp_len = 3;
    f_req_outcome.negative = false;
    f_req_outcome.pending_count = 2; /* two 0x78 consumed downstream */

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));

    uint8_t req[] = { 0x22, 0xF1, 0x90 };
    uint8_t resp[16];
    size_t n = 0;
    se_obd_outcome_t out;

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK,
                          se_obd_request(req, sizeof(req), 500, resp,
                                         sizeof(resp), &n, &out));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_HEX8(0x62, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(2, out.pending_count);
    TEST_ASSERT_EQUAL_UINT32(0x7E0, f_last_addr.tx_id); /* claim addr used */
}

void test_obd_request_negative_response(void)
{
    reset_fakes();

    f_resp[0] = 0x7F; f_resp[1] = 0x22; f_resp[2] = 0x31;
    f_resp_len = 3;
    f_req_outcome.negative = true;
    f_req_outcome.nrc = 0x31;

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));

    uint8_t req[] = { 0x22, 0xFF, 0xFF };
    uint8_t resp[16];
    size_t n = 0;
    se_obd_outcome_t out;

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK,
                          se_obd_request(req, sizeof(req), 0, resp,
                                         sizeof(resp), &n, &out));
    TEST_ASSERT_TRUE(out.negative);
    TEST_ASSERT_EQUAL_HEX8(0x31, out.nrc);
}

void test_obd_isotp_requires_claim(void)
{
    reset_fakes();

    uint8_t d[] = { 0x3E, 0x80 };
    uint8_t buf[8];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_NOCLAIM,
                          se_obd_isotp_tx(d, sizeof(d), 100));
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_NOCLAIM,
                          se_obd_isotp_rx(buf, sizeof(buf), &n, 100));
    TEST_ASSERT_EQUAL_INT(0, f_tx_calls);
    TEST_ASSERT_EQUAL_INT(0, f_rx_calls);
}

void test_obd_isotp_under_claim(void)
{
    reset_fakes();

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A2));

    uint8_t d[] = { 0x10, 0x03 };
    uint8_t buf[8];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_isotp_tx(d, sizeof(d), 100));
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK,
                          se_obd_isotp_rx(buf, sizeof(buf), &n, 100));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x7E, buf[0]);
    TEST_ASSERT_EQUAL_UINT32(0x7E1, f_last_addr.tx_id);
}

void test_obd_bad_args(void)
{
    reset_fakes();

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG, se_obd_claim(NULL));

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A1));

    uint8_t resp[8];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG,
                          se_obd_request(NULL, 0, 0, resp, sizeof(resp),
                                         &n, NULL));
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG, se_obd_isotp_tx(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG,
                          se_obd_isotp_rx(NULL, 0, NULL, 0));
}
