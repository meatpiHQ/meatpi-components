/**
 * @file test_translator_contract.c
 * @brief Proves the bridge_translator_t contract (ctx/sink plumbing) with a
 *        line-splitter test codec: decode reassembles '\n'-terminated lines
 *        from arbitrarily fragmented input and emits one sink call per
 *        complete line — zero, one, or many outputs per input chunk, exactly
 *        the shape slcan/GVRET reassembly needs.
 */
#include <string.h>

#include "unity.h"

#include "bridge_manager.h"

/* ---- the test codec ------------------------------------------------------------ */

typedef struct
{
    uint8_t buf[64];
    size_t  len;
} splitter_ctx_t;

static esp_err_t splitter_ctx_init(void *ctx)
{
    memset(ctx, 0, sizeof(splitter_ctx_t));
    return ESP_OK;
}

static esp_err_t splitter_decode(void *vctx, const uint8_t *in, size_t len,
                                 bridge_sink_fn_t sink, void *sink_arg)
{
    splitter_ctx_t *ctx = vctx;

    for (size_t i = 0; i < len; i++)
    {
        if (ctx->len < sizeof(ctx->buf))
        {
            ctx->buf[ctx->len++] = in[i];
        }

        if (in[i] == '\n')
        {
            esp_err_t err = sink(sink_arg, ctx->buf, ctx->len);

            ctx->len = 0;

            if (err != ESP_OK)
            {
                return err;
            }
        }
    }

    return ESP_OK;
}

/* encode: passthrough (asymmetric codecs are legitimate) */
static esp_err_t splitter_encode(void *vctx, const uint8_t *in, size_t len,
                                 bridge_sink_fn_t sink, void *sink_arg)
{
    (void)vctx;
    return sink(sink_arg, in, len);
}

static const bridge_translator_t SPLITTER =
{
    .name = "splitter",
    .ctx_size = sizeof(splitter_ctx_t),
    .ctx_init = splitter_ctx_init,
    .decode = splitter_decode,
    .encode = splitter_encode,
};

/* ---- sink capture ---------------------------------------------------------------- */

static char   s_out[8][64];
static size_t s_out_len[8];
static int    s_out_count;

static esp_err_t capture_sink(void *arg, const uint8_t *out, size_t out_len)
{
    (void)arg;

    if (s_out_count < 8 && out_len < sizeof(s_out[0]))
    {
        memcpy(s_out[s_out_count], out, out_len);
        s_out[s_out_count][out_len] = '\0';
        s_out_len[s_out_count] = out_len;
        s_out_count++;
    }

    return ESP_OK;
}

static void reset_capture(void)
{
    s_out_count = 0;
    memset(s_out, 0, sizeof(s_out));
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_ctx_size_fits_manager_pool(void)
{
    TEST_ASSERT_LESS_OR_EQUAL(256, (int)SPLITTER.ctx_size); /* BM_CTX_MAX */
}

static void test_one_input_many_outputs(void)
{
    splitter_ctx_t ctx;

    splitter_ctx_init(&ctx);
    reset_capture();

    const char *in = "one\ntwo\nthree\n";

    TEST_ASSERT_EQUAL(ESP_OK, splitter_decode(&ctx, (const uint8_t *)in,
                                              strlen(in), capture_sink,
                                              NULL));
    TEST_ASSERT_EQUAL(3, s_out_count);
    TEST_ASSERT_EQUAL_STRING("one\n", s_out[0]);
    TEST_ASSERT_EQUAL_STRING("three\n", s_out[2]);
}

static void test_fragmented_input_reassembles(void)
{
    splitter_ctx_t ctx;

    splitter_ctx_init(&ctx);
    reset_capture();

    /* one frame split across three chunks (the chunk-boundary bug class) */
    splitter_decode(&ctx, (const uint8_t *)"he", 2, capture_sink, NULL);
    TEST_ASSERT_EQUAL(0, s_out_count); /* zero outputs mid-frame */
    splitter_decode(&ctx, (const uint8_t *)"ll", 2, capture_sink, NULL);
    splitter_decode(&ctx, (const uint8_t *)"o\n", 2, capture_sink, NULL);
    TEST_ASSERT_EQUAL(1, s_out_count);
    TEST_ASSERT_EQUAL_STRING("hello\n", s_out[0]);
}

static void test_ctx_isolation_between_directions(void)
{
    /* two ctx instances (a2b vs b2a in the pump) must not share state */
    splitter_ctx_t ctx1;
    splitter_ctx_t ctx2;

    splitter_ctx_init(&ctx1);
    splitter_ctx_init(&ctx2);
    reset_capture();

    splitter_decode(&ctx1, (const uint8_t *)"par", 3, capture_sink, NULL);
    splitter_decode(&ctx2, (const uint8_t *)"x\n", 2, capture_sink, NULL);
    TEST_ASSERT_EQUAL(1, s_out_count);
    TEST_ASSERT_EQUAL_STRING("x\n", s_out[0]); /* ctx1's partial untouched */

    splitter_decode(&ctx1, (const uint8_t *)"tial\n", 5, capture_sink, NULL);
    TEST_ASSERT_EQUAL(2, s_out_count);
    TEST_ASSERT_EQUAL_STRING("partial\n", s_out[1]);
}

void run_contract_tests(void)
{
    RUN_TEST(test_ctx_size_fits_manager_pool);
    RUN_TEST(test_one_input_many_outputs);
    RUN_TEST(test_fragmented_input_reassembles);
    RUN_TEST(test_ctx_isolation_between_directions);
}
