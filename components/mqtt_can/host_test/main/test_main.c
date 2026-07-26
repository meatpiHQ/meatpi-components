/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/** @file test_main.c — mqtt_can pure-codec suite (TASK_mqtt_can.md §7). */
#include <string.h>

#include "unity.h"

#include "can_frame_wire.h"
#include "mqtt_can_codec.h"

/* ---- helpers ---------------------------------------------------------------- */

static can_core_frame_t mk_frame(uint32_t id, uint8_t dlc, bool ext,
                                 bool rtr)
{
    can_core_frame_t f;

    memset(&f, 0, sizeof(f));
    f.id = id;
    f.dlc = dlc;
    f.ext = ext;
    f.rtr = rtr;

    for (uint8_t i = 0; i < dlc; i++)
    {
        f.data[i] = (uint8_t)(i + 1);
    }

    return f;
}

static size_t wire_chunk(const can_core_frame_t *frames, int n,
                         uint8_t *out)
{
    size_t w = 0;

    for (int i = 0; i < n; i++)
    {
        w += can_wire_encode(&frames[i], out + w);
    }

    return w;
}

/* ---- rx: batch -> legacy JSON ---------------------------------------------- */

void test_batch_json_legacy_exact(void)
{
    mc_batch_t b;
    uint8_t chunk[64];
    char json[MC_JSON_MAX];
    can_core_frame_t f = mk_frame(123, 8, false, false);

    mc_batch_init(&b, 4);
    TEST_ASSERT_EQUAL(1, mc_batch_add_chunk(&b, chunk,
                                            wire_chunk(&f, 1, chunk)));
    TEST_ASSERT_FALSE(mc_batch_ready(&b));

    size_t n = mc_batch_json(&b, 34610, json, sizeof(json));

    /* byte parity with the legacy contract (legacy main/mqtt.c:209) */
    TEST_ASSERT_EQUAL_STRING(
        "{\"bus\":\"0\",\"type\":\"rx\",\"ts\":34610,\"frame\":"
        "[{\"id\":123,\"dlc\":8,\"rtr\":false,\"extd\":false,"
        "\"data\":[1,2,3,4,5,6,7,8]}]}", json);
    TEST_ASSERT_EQUAL(strlen(json), n);
    TEST_ASSERT_EQUAL(0, b.n);          /* drained                      */
    TEST_ASSERT_EQUAL(0, mc_batch_json(&b, 1, json, sizeof(json)));
}

void test_batch_json_multi_ext_rtr(void)
{
    mc_batch_t b;
    uint8_t chunk[64];
    char json[MC_JSON_MAX];
    can_core_frame_t fr[2] =
    {
        mk_frame(0x1ABCDEF0, 2, true, false),
        mk_frame(0x100, 0, false, true),
    };

    mc_batch_init(&b, 2);
    TEST_ASSERT_EQUAL(2, mc_batch_add_chunk(&b, chunk,
                                            wire_chunk(fr, 2, chunk)));
    TEST_ASSERT_TRUE(mc_batch_ready(&b));

    (void)mc_batch_json(&b, 7, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING(
        "{\"bus\":\"0\",\"type\":\"rx\",\"ts\":7,\"frame\":"
        "[{\"id\":448585456,\"dlc\":2,\"rtr\":false,\"extd\":true,"
        "\"data\":[1,2]},"
        "{\"id\":256,\"dlc\":0,\"rtr\":true,\"extd\":false,"
        "\"data\":[]}]}", json);
}

void test_batch_overflow_counted(void)
{
    mc_batch_t b;
    uint8_t chunk[CAN_WIRE_MAX];
    can_core_frame_t f = mk_frame(1, 8, false, false);
    size_t len = wire_chunk(&f, 1, chunk);

    mc_batch_init(&b, MC_BATCH_MAX);

    for (int i = 0; i < MC_BATCH_MAX + 5; i++)
    {
        mc_batch_add_chunk(&b, chunk, len);
    }

    TEST_ASSERT_EQUAL(MC_BATCH_MAX, b.n);
    TEST_ASSERT_EQUAL(5, b.dropped);    /* nothing vanishes uncounted   */

    char json[MC_JSON_MAX];
    size_t n = mc_batch_json(&b, 1, json, sizeof(json));

    TEST_ASSERT_TRUE(n > 0 && n < sizeof(json)); /* full batch FITS     */
}

void test_batch_worstcase_fits(void)
{
    /* 24 max-size frames (29-bit id, 8 data bytes of 255) must fit
       MC_JSON_MAX — the sizing contract behind BM_CTX_MAX */
    mc_batch_t b;
    uint8_t chunk[CAN_WIRE_MAX];
    char json[MC_JSON_MAX];
    can_core_frame_t f = mk_frame(0x1FFFFFFF, 8, true, false);

    memset(f.data, 255, 8);
    mc_batch_init(&b, MC_BATCH_MAX);

    for (int i = 0; i < MC_BATCH_MAX; i++)
    {
        mc_batch_add_chunk(&b, chunk, wire_chunk(&f, 1, chunk));
    }

    size_t n = mc_batch_json(&b, 9223372036854775807LL, json,
                             sizeof(json));

    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(n < MC_JSON_MAX);
}

/* ---- tx: stream reassembly -------------------------------------------------- */

static char g_obj[MC_TXBUF_MAX + 1];
static int g_objs;

static void obj_cb(void *arg, const char *json, size_t len)
{
    (void)arg;
    memcpy(g_obj, json, len);
    g_obj[len] = '\0';
    g_objs++;
}

void test_txasm_fragmented(void)
{
    mc_txasm_t a;
    const char *msg =
        "{\"bus\":0,\"type\":\"tx\",\"frame\":[{\"id\":1,\"dlc\":1,"
        "\"data\":[9]}]}";
    size_t len = strlen(msg);

    mc_txasm_init(&a);
    g_objs = 0;

    /* feed one byte at a time — worst-case fragmentation */
    for (size_t i = 0; i < len; i++)
    {
        mc_txasm_feed(&a, (const uint8_t *)msg + i, 1, obj_cb, NULL);
    }

    TEST_ASSERT_EQUAL(1, g_objs);
    TEST_ASSERT_EQUAL_STRING(msg, g_obj);
}

void test_txasm_noise_braces_in_strings(void)
{
    mc_txasm_t a;
    const char *stream =
        "garbage {\"a\":\"}{\\\"\",\"b\":{\"c\":1}} trailing"
        "{\"d\":2}";

    mc_txasm_init(&a);
    g_objs = 0;
    mc_txasm_feed(&a, (const uint8_t *)stream, strlen(stream), obj_cb,
                  NULL);
    TEST_ASSERT_EQUAL(2, g_objs);
    TEST_ASSERT_EQUAL_STRING("{\"d\":2}", g_obj);
}

void test_txasm_oversize_discarded_counted(void)
{
    mc_txasm_t a;
    static char big[MC_TXBUF_MAX + 64];
    size_t w = 0;

    big[w++] = '{';
    big[w++] = '"';
    big[w++] = 'x';
    big[w++] = '"';
    big[w++] = ':';
    big[w++] = '"';

    while (w < MC_TXBUF_MAX + 40)
    {
        big[w++] = 'a';
    }

    big[w++] = '"';
    big[w++] = '}';

    mc_txasm_init(&a);
    g_objs = 0;
    mc_txasm_feed(&a, (const uint8_t *)big, w, obj_cb, NULL);
    TEST_ASSERT_EQUAL(0, g_objs);
    TEST_ASSERT_EQUAL(1, a.oversize);

    /* the stream recovers: a well-formed object after the monster */
    const char *ok = "{\"d\":3}";

    mc_txasm_feed(&a, (const uint8_t *)ok, strlen(ok), obj_cb, NULL);
    TEST_ASSERT_EQUAL(1, g_objs);
    TEST_ASSERT_EQUAL_STRING(ok, g_obj);
}

/* ---- tx: parse -------------------------------------------------------------- */

void test_tx_parse_legacy_exact(void)
{
    /* the documented legacy tx example (legacy main/mqtt.c:212) */
    const char *msg =
        "{\"bus\":0,\"type\":\"tx\",\"frame\":[{\"id\":123,\"dlc\":8,"
        "\"rtr\":false,\"extd\":true,"
        "\"data\":[1,2,3,4,5,6,7,8]},{\"id\":124,\"dlc\":8,"
        "\"rtr\":false,\"extd\":true,"
        "\"data\":[1,2,3,4,5,6,7,8]}]}";
    can_core_frame_t out[4];
    int n = mc_tx_parse(msg, strlen(msg), out, 4);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(123, out[0].id);
    TEST_ASSERT_TRUE(out[0].ext);
    TEST_ASSERT_FALSE(out[0].rtr);
    TEST_ASSERT_EQUAL(8, out[0].dlc);
    TEST_ASSERT_EQUAL_HEX8(8, out[0].data[7]);
    TEST_ASSERT_EQUAL_HEX32(124, out[1].id);
}

void test_tx_parse_fuel_level_example(void)
{
    /* legacy main/mqtt.c:215 — the "get fuel level" recipe, with ts */
    const char *msg =
        "{\"bus\":0,\"type\":\"tx\",\"ts\":35519,\"frame\":"
        "[{\"id\":2016,\"dlc\":8,\"rtr\":false,\"extd\":false,"
        "\"data\":[2,1,47,170,170,170,170,170]}]}";
    can_core_frame_t out[1];
    int n = mc_tx_parse(msg, strlen(msg), out, 1);

    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(2016, out[0].id);
    TEST_ASSERT_FALSE(out[0].ext);
    TEST_ASSERT_EQUAL_HEX8(47, out[0].data[2]);
}

void test_tx_parse_rejects(void)
{
    can_core_frame_t out[4];

    /* wrong type */
    const char *rx = "{\"type\":\"rx\",\"frame\":[]}";

    TEST_ASSERT_EQUAL(-1, mc_tx_parse(rx, strlen(rx), out, 4));

    /* dlc out of range */
    const char *dlc =
        "{\"type\":\"tx\",\"frame\":[{\"id\":1,\"dlc\":9,\"data\":[]}]}";

    TEST_ASSERT_EQUAL(-1, mc_tx_parse(dlc, strlen(dlc), out, 4));

    /* id beyond 29 bits */
    const char *id =
        "{\"type\":\"tx\",\"frame\":[{\"id\":536870912,\"dlc\":0}]}";

    TEST_ASSERT_EQUAL(-1, mc_tx_parse(id, strlen(id), out, 4));

    /* not JSON at all */
    TEST_ASSERT_EQUAL(-1, mc_tx_parse("junk", 4, out, 4));

    /* empty frame array is VALID (0 frames) */
    const char *empty = "{\"type\":\"tx\",\"frame\":[]}";

    TEST_ASSERT_EQUAL(0, mc_tx_parse(empty, strlen(empty), out, 4));
}

void test_tx_parse_array_cap_and_implicit_ext(void)
{
    /* 3 frames into a 2-slot array: keep what fits; id > 0x7FF gets
       ext even without an "extd" key */
    const char *msg =
        "{\"type\":\"tx\",\"frame\":[{\"id\":2047,\"dlc\":0},"
        "{\"id\":2048,\"dlc\":0},{\"id\":3,\"dlc\":0}]}";
    can_core_frame_t out[2];
    int n = mc_tx_parse(msg, strlen(msg), out, 2);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FALSE(out[0].ext);      /* 0x7FF still standard        */
    TEST_ASSERT_TRUE(out[1].ext);       /* 0x800 implies extended      */
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_batch_json_legacy_exact);
    RUN_TEST(test_batch_json_multi_ext_rtr);
    RUN_TEST(test_batch_overflow_counted);
    RUN_TEST(test_batch_worstcase_fits);

    RUN_TEST(test_txasm_fragmented);
    RUN_TEST(test_txasm_noise_braces_in_strings);
    RUN_TEST(test_txasm_oversize_discarded_counted);

    RUN_TEST(test_tx_parse_legacy_exact);
    RUN_TEST(test_tx_parse_fuel_level_example);
    RUN_TEST(test_tx_parse_rejects);
    RUN_TEST(test_tx_parse_array_cap_and_implicit_ext);

    UNITY_END();
}
