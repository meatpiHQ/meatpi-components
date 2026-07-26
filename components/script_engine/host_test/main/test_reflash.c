/**
 * @file test_reflash.c
 * @brief Host suite for se_obd_transfer_file (the UDS TransferData
 *        streamer behind obd_transfer_file) over a fake port + a
 *        byte-array file reader. Covers block framing, bsc increment +
 *        wrap, short last block, CRC-32 correctness, NRC rejection, and
 *        the read-failure path. The allow_reflash GATE lives in the
 *        Berry binding (needs settings + the VM) and is verified live
 *        on the bench, not here.
 */
#include <string.h>

#include "unity.h"

#include "script_engine_obd.h"

/* ---- fake TransferData transport (echoes 76 <bsc>) ------------------------ */

static int      r_calls;
static size_t   r_payload;      /* total block bytes seen (minus 36+bsc)     */
static uint8_t  r_seen_bsc[64]; /* bsc of each block, in order               */
static size_t   r_last_block;   /* size of the most recent block payload     */
static int      r_neg_at;       /* 1-based call index to answer negative (-1)*/

static int rf_begin(const se_obd_addr_t *a) { (void)a; return 0; }
static void rf_end(void) {}

static int rf_request(const se_obd_addr_t *a, const uint8_t *req,
                      size_t req_len, uint32_t t, uint8_t *resp,
                      size_t cap, size_t *rn, se_obd_outcome_t *out)
{
    (void)a; (void)t; (void)cap;
    r_calls++;

    if (out != NULL)
    {
        memset(out, 0, sizeof(*out));
    }

    /* req = 36 <bsc> <block...> */
    if (req_len >= 2 && r_calls <= (int)(sizeof(r_seen_bsc)))
    {
        r_seen_bsc[r_calls - 1] = req[1];
    }

    r_last_block = req_len - 2;
    r_payload += req_len - 2;

    if (r_neg_at == r_calls)
    {
        resp[0] = 0x7F; resp[1] = 0x36; resp[2] = 0x72; /* programmingFailure */
        *rn = 3;
        if (out != NULL) { out->negative = true; out->nrc = 0x72; }
        return 0;
    }

    resp[0] = 0x76; resp[1] = req[1]; /* positive, echo bsc */
    *rn = 2;
    return 0;
}

static int rf_tx(const se_obd_addr_t *a, const uint8_t *d, size_t n,
                 uint32_t t) { (void)a; (void)d; (void)n; (void)t; return 0; }
static int rf_rx(const se_obd_addr_t *a, uint8_t *o, size_t c, size_t *n,
                 uint32_t t) { (void)a; (void)c; (void)t; o[0] = 0; *n = 1;
                               return 0; }

static const se_obd_port_t RFPORT =
{
    .session_begin = rf_begin,
    .session_end   = rf_end,
    .request       = rf_request,
    .isotp_tx      = rf_tx,
    .isotp_rx      = rf_rx,
};

/* ---- fake file reader over a byte array ---------------------------------- */

typedef struct
{
    const uint8_t *data;
    size_t         size;
    long           fail_at; /* offset to fail the read at, or -1        */
} arr_ctx_t;

static int arr_read(void *vctx, size_t off, uint8_t *out, size_t len)
{
    arr_ctx_t *c = vctx;

    if (c->fail_at >= 0 && off + len > (size_t)c->fail_at)
    {
        return -1; /* simulate a short/failed read */
    }

    if (off + len > c->size)
    {
        return -1;
    }

    memcpy(out, c->data + off, len);
    return (int)len;
}

static void reset(void)
{
    r_calls = 0;
    r_payload = 0;
    r_last_block = 0;
    r_neg_at = -1;
    memset(r_seen_bsc, 0, sizeof(r_seen_bsc));
    se_obd_set_port(&RFPORT);
}

static const se_obd_addr_t A = { .tx_id = 0x7E0, .rx_id = 0x7E8 };

/* ---- tests --------------------------------------------------------------- */

void test_xfer_requires_claim(void)
{
    reset();
    uint8_t fw[8] = { 0 };
    arr_ctx_t c = { fw, sizeof(fw), -1 };

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_NOCLAIM,
        se_obd_transfer_file(arr_read, &c, 8, 8, 1, 100, NULL));
    TEST_ASSERT_EQUAL_INT(0, r_calls);
}

void test_xfer_blocks_and_bsc(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A));

    uint8_t fw[200];
    for (int i = 0; i < 200; i++) { fw[i] = (uint8_t)(i * 7 + 3); }
    arr_ctx_t c = { fw, sizeof(fw), -1 };

    se_obd_xfer_result_t res;
    int r = se_obd_transfer_file(arr_read, &c, 200, 32, 1, 100, &res);

    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, r);
    TEST_ASSERT_EQUAL_size_t(200, res.sent);
    TEST_ASSERT_EQUAL_UINT32(7, res.blocks);   /* 6*32 + 8 */
    TEST_ASSERT_EQUAL_UINT32(200, r_payload);
    TEST_ASSERT_EQUAL_UINT8(1, r_seen_bsc[0]); /* first bsc = 1 */
    TEST_ASSERT_EQUAL_UINT8(7, r_seen_bsc[6]); /* 7th bsc = 7 */
    TEST_ASSERT_EQUAL_size_t(8, r_last_block); /* short last block */
}

void test_xfer_bsc_wraps(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A));

    uint8_t fw[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    arr_ctx_t c = { fw, sizeof(fw), -1 };

    /* first_bsc 0xFE, 1-byte blocks -> FE, FF, 00 */
    se_obd_xfer_result_t res;
    se_obd_transfer_file(arr_read, &c, 3, 1, 0xFE, 100, &res);

    TEST_ASSERT_EQUAL_UINT8(0xFE, r_seen_bsc[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, r_seen_bsc[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, r_seen_bsc[2]);
}

void test_xfer_crc32_check_value(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A));

    /* canonical CRC-32 check vector: "123456789" -> 0xCBF43926 */
    const uint8_t v[] = "123456789";
    arr_ctx_t c = { v, 9, -1 };

    se_obd_xfer_result_t res;
    se_obd_transfer_file(arr_read, &c, 9, 9, 1, 100, &res);

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, res.crc32);
}

void test_xfer_negative_response_stops(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A));

    uint8_t fw[128] = { 0 };
    arr_ctx_t c = { fw, sizeof(fw), -1 };
    r_neg_at = 2; /* second block rejected */

    se_obd_xfer_result_t res;
    int r = se_obd_transfer_file(arr_read, &c, 128, 32, 1, 100, &res);

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_IO, r);
    TEST_ASSERT_EQUAL_UINT32(2, res.blocks);   /* stopped at block 2 */
    TEST_ASSERT_NOT_NULL(res.msg);
}

void test_xfer_read_failure(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A));

    uint8_t fw[128] = { 0 };
    arr_ctx_t c = { fw, sizeof(fw), 40 }; /* fail once past offset 40 */

    se_obd_xfer_result_t res;
    int r = se_obd_transfer_file(arr_read, &c, 128, 32, 1, 100, &res);

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_IO, r);
    TEST_ASSERT_NOT_NULL(res.msg);
}

void test_xfer_bad_args(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(SE_OBD_OK, se_obd_claim(&A));

    uint8_t fw[8] = { 0 };
    arr_ctx_t c = { fw, sizeof(fw), -1 };

    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG,
        se_obd_transfer_file(NULL, &c, 8, 8, 1, 100, NULL));      /* no reader */
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG,
        se_obd_transfer_file(arr_read, &c, 0, 8, 1, 100, NULL));  /* size 0 */
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG,
        se_obd_transfer_file(arr_read, &c, 8, 0, 1, 100, NULL));  /* block 0 */
    TEST_ASSERT_EQUAL_INT(SE_OBD_ERR_ARG,
        se_obd_transfer_file(arr_read, &c, 8, 99999, 1, 100, NULL)); /* > max */
}
