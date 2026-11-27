/*
 * This file is part of the MeatPi components project.
 * Copyright (C) 2022-2026 MeatPi Electronics.
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

/* The pure parts of ECU flashing: ISO-TP frames (script_engine_tp_core.c),
 * the device lock's rules and the checksum helpers
 * (script_engine_flash_core.c). */
#include <string.h>

#include "unity.h"

#include "script_engine_flash_core.h"
#include "script_engine_tp_core.h"

/* ---- ISO-TP ---------------------------------------------------------------- */

void test_tp_single_frame_padded(void)
{
    se_tp_opts_t o;
    uint8_t fr[8];
    size_t used = 0;
    const uint8_t msg[] = { 0x27, 0x01 };

    se_tp_opts_default(&o);
    o.pad_byte = 0xAA;

    TEST_ASSERT_EQUAL_UINT8(8, se_tp_first(&o, msg, sizeof(msg), fr, &used));
    TEST_ASSERT_EQUAL_size_t(2, used);

    const uint8_t want[8] = { 0x02, 0x27, 0x01, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };

    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, fr, 8);
}

void test_tp_extended_address_single(void)
{
    /* the Toyota gateway: 0x750, AE 5F, "27 51" -> 5F 02 27 51 */
    se_tp_opts_t o;
    uint8_t fr[8];
    size_t used = 0;
    const uint8_t msg[] = { 0x27, 0x51 };

    se_tp_opts_default(&o);
    o.ae_tx = 0x5F;
    o.pad = false;

    TEST_ASSERT_EQUAL_UINT8(4, se_tp_first(&o, msg, sizeof(msg), fr, &used));

    const uint8_t want[4] = { 0x5F, 0x02, 0x27, 0x51 };

    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, fr, 4);
}

void test_tp_multi_frame_segmentation(void)
{
    se_tp_opts_t o;
    uint8_t msg[1024];
    uint8_t fr[8];
    uint8_t back[1024];
    size_t off = 0;

    for (size_t i = 0; i < sizeof(msg); i++)
    {
        msg[i] = (uint8_t)(i * 7);
    }

    se_tp_opts_default(&o);

    /* first frame: 14 00 + 6 bytes, then 146 CFs (145 of 7, the last of 3):
       147 frames, as a Toyota loader piece of 1024 bytes */
    TEST_ASSERT_EQUAL_UINT8(8, se_tp_first(&o, msg, sizeof(msg), fr, &off));
    TEST_ASSERT_EQUAL_HEX8(0x14, fr[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, fr[1]);
    TEST_ASSERT_EQUAL_size_t(6, off);

    se_tp_rx_t rx;
    se_tp_opts_t ro;

    se_tp_opts_default(&ro);
    se_tp_rx_init(&rx, back, sizeof(back));
    TEST_ASSERT_EQUAL(SE_TP_RX_NEED_FC, se_tp_rx_feed(&rx, &ro, fr, 8));

    int frames = 1;
    uint8_t sn = 1;
    se_tp_rx_status_t st = SE_TP_RX_MORE;

    while (off < sizeof(msg))
    {
        uint8_t dlc = se_tp_consecutive(&o, msg, sizeof(msg), &off, sn, fr);

        TEST_ASSERT_EQUAL_UINT8(8, dlc);
        TEST_ASSERT_EQUAL_HEX8(0x20 | sn, fr[0]);
        st = se_tp_rx_feed(&rx, &ro, fr, dlc);
        sn = (uint8_t)((sn + 1) & 0x0F);
        frames++;
    }

    TEST_ASSERT_EQUAL_INT(147, frames);
    TEST_ASSERT_EQUAL(SE_TP_RX_DONE, st);
    TEST_ASSERT_EQUAL_size_t(sizeof(msg), rx.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, back, sizeof(msg));
}

void test_tp_extended_address_round_trip(void)
{
    se_tp_opts_t tx;
    se_tp_opts_t rxo;
    uint8_t msg[20];
    uint8_t back[64];
    uint8_t fr[8];
    size_t off = 0;
    se_tp_rx_t rx;

    for (size_t i = 0; i < sizeof(msg); i++)
    {
        msg[i] = (uint8_t)i;
    }

    se_tp_opts_default(&tx);
    tx.ae_tx = 0x67;
    se_tp_opts_default(&rxo);
    rxo.ae_rx = 0x67;
    se_tp_rx_init(&rx, back, sizeof(back));

    se_tp_first(&tx, msg, sizeof(msg), fr, &off);
    TEST_ASSERT_EQUAL_HEX8(0x67, fr[0]);
    TEST_ASSERT_EQUAL_size_t(5, off); /* AE + 2 PCI bytes leave 5 */
    TEST_ASSERT_EQUAL(SE_TP_RX_NEED_FC, se_tp_rx_feed(&rx, &rxo, fr, 8));

    uint8_t sn = 1;
    se_tp_rx_status_t st = SE_TP_RX_MORE;

    while (off < sizeof(msg))
    {
        uint8_t dlc = se_tp_consecutive(&tx, msg, sizeof(msg), &off, sn++, fr);

        st = se_tp_rx_feed(&rx, &rxo, fr, dlc);
    }

    TEST_ASSERT_EQUAL(SE_TP_RX_DONE, st);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, back, sizeof(msg));

    /* a frame with another AE byte is not ours */
    fr[0] = 0x5F;
    TEST_ASSERT_EQUAL(SE_TP_RX_IGNORED, se_tp_rx_feed(&rx, &rxo, fr, 8));
}

void test_tp_sequence_error_and_overflow(void)
{
    se_tp_opts_t o;
    uint8_t back[16];
    se_tp_rx_t rx;

    se_tp_opts_default(&o);
    se_tp_rx_init(&rx, back, sizeof(back));

    const uint8_t ff[8] = { 0x10, 0x0A, 1, 2, 3, 4, 5, 6 };
    const uint8_t cf_bad[8] = { 0x22, 7, 8, 9, 10, 0, 0, 0 };
    const uint8_t ff_big[8] = { 0x10, 0x40, 0, 0, 0, 0, 0, 0 };

    TEST_ASSERT_EQUAL(SE_TP_RX_NEED_FC, se_tp_rx_feed(&rx, &o, ff, 8));
    TEST_ASSERT_EQUAL(SE_TP_RX_ERROR, se_tp_rx_feed(&rx, &o, cf_bad, 8));
    TEST_ASSERT_EQUAL(SE_TP_RX_ERROR, se_tp_rx_feed(&rx, &o, ff_big, 8));
    /* a CF with no first frame before it is someone else's */
    TEST_ASSERT_EQUAL(SE_TP_RX_IGNORED, se_tp_rx_feed(&rx, &o, cf_bad, 8));
}

void test_tp_flow_control_parse(void)
{
    se_tp_opts_t o;
    uint8_t bs = 0;
    uint32_t us = 0;

    se_tp_opts_default(&o);

    const uint8_t cts[8] = { 0x30, 0x08, 0xF3, 0, 0, 0, 0, 0 };
    const uint8_t wait[8] = { 0x31, 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t ovf[8] = { 0x32, 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t data[8] = { 0x02, 0x67, 0x01, 0, 0, 0, 0, 0 };

    TEST_ASSERT_EQUAL(SE_TP_FC_CTS, se_tp_parse_fc(&o, cts, 8, &bs, &us));
    TEST_ASSERT_EQUAL_UINT8(8, bs);
    TEST_ASSERT_EQUAL_UINT32(300, us);
    TEST_ASSERT_EQUAL(SE_TP_FC_WAIT, se_tp_parse_fc(&o, wait, 8, &bs, &us));
    TEST_ASSERT_EQUAL(SE_TP_FC_OVERFLOW, se_tp_parse_fc(&o, ovf, 8, &bs, &us));
    TEST_ASSERT_EQUAL(SE_TP_FC_NOT_FC, se_tp_parse_fc(&o, data, 8, &bs, &us));

    TEST_ASSERT_EQUAL_UINT32(0, se_tp_stmin_us(0x00));
    TEST_ASSERT_EQUAL_UINT32(127000, se_tp_stmin_us(0x7F));
    TEST_ASSERT_EQUAL_UINT32(100, se_tp_stmin_us(0xF1));
    TEST_ASSERT_EQUAL_UINT32(900, se_tp_stmin_us(0xF9));
    TEST_ASSERT_EQUAL_UINT32(127000, se_tp_stmin_us(0x80)); /* reserved */

    uint8_t fc[8];

    o.bs = 4;
    o.stmin = 0x01;
    TEST_ASSERT_EQUAL_UINT8(8, se_tp_flow_control(&o, fc));
    TEST_ASSERT_EQUAL_HEX8(0x30, fc[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, fc[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, fc[2]);
}

void test_tp_block_size_flow_control(void)
{
    /* we advertise BS 2: after every 2 CFs the sender waits for our FC */
    se_tp_opts_t o;
    uint8_t msg[30];
    uint8_t back[64];
    uint8_t fr[8];
    size_t off = 0;
    se_tp_rx_t rx;

    for (size_t i = 0; i < sizeof(msg); i++)
    {
        msg[i] = (uint8_t)(0x40 + i);
    }

    se_tp_opts_default(&o);
    o.bs = 2;
    se_tp_rx_init(&rx, back, sizeof(back));

    se_tp_first(&o, msg, sizeof(msg), fr, &off);
    TEST_ASSERT_EQUAL(SE_TP_RX_NEED_FC, se_tp_rx_feed(&rx, &o, fr, 8));

    const se_tp_rx_status_t want[4] =
    {
        SE_TP_RX_MORE, SE_TP_RX_NEED_FC, SE_TP_RX_MORE, SE_TP_RX_DONE,
    };
    uint8_t sn = 1;

    for (int i = 0; i < 4; i++)
    {
        uint8_t dlc = se_tp_consecutive(&o, msg, sizeof(msg), &off, sn++, fr);

        TEST_ASSERT_EQUAL(want[i], se_tp_rx_feed(&rx, &o, fr, dlc));
    }

    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, back, sizeof(msg));
}

void test_tp_rejects_bad_lengths(void)
{
    se_tp_opts_t o;
    uint8_t fr[8];
    size_t used;
    static uint8_t big[SE_TP_MAX + 1];

    se_tp_opts_default(&o);
    TEST_ASSERT_EQUAL_UINT8(0, se_tp_first(&o, big, 0, fr, &used));
    TEST_ASSERT_EQUAL_UINT8(0, se_tp_first(&o, big, SE_TP_MAX + 1, fr, &used));
    TEST_ASSERT_EQUAL_UINT8(8, se_tp_first(&o, big, SE_TP_MAX, fr, &used));
    TEST_ASSERT_EQUAL_HEX8(0x1F, fr[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, fr[1]);
}

/* ---- the lock ---------------------------------------------------------------- */

void test_lock_lifecycle(void)
{
    se_lock_state_t s = se_lock_on_boot(false);

    TEST_ASSERT_EQUAL(SE_LOCK_IDLE, s);
    TEST_ASSERT_FALSE(se_lock_held(s));

    /* a run that never commits leaves it idle, well or not */
    TEST_ASSERT_EQUAL(SE_LOCK_IDLE, se_lock_on_run_end(s, false, false));

    s = se_lock_on_commit(s);
    TEST_ASSERT_EQUAL(SE_LOCK_COMMITTED, s);
    TEST_ASSERT_TRUE(se_lock_held(s));
    /* a running job is never released */
    TEST_ASSERT_EQUAL(SE_LOCK_COMMITTED, se_lock_on_release(s));
    TEST_ASSERT_EQUAL(SE_LOCK_IDLE, se_lock_on_run_end(s, true, false));
    /* a run that returned after an abort request left a half-written ECU */
    TEST_ASSERT_EQUAL(SE_LOCK_FAILED, se_lock_on_run_end(s, true, true));

    s = se_lock_on_run_end(SE_LOCK_COMMITTED, false, false);
    TEST_ASSERT_EQUAL(SE_LOCK_FAILED, s);
    /* a recovery run commits again and can end the lock */
    TEST_ASSERT_EQUAL(SE_LOCK_COMMITTED, se_lock_on_commit(s));
    TEST_ASSERT_EQUAL(SE_LOCK_IDLE, se_lock_on_release(s));

    s = se_lock_on_boot(true);
    TEST_ASSERT_EQUAL(SE_LOCK_RECOVERY, s);
    TEST_ASSERT_EQUAL_STRING("recovery", se_lock_state_str(s));
    TEST_ASSERT_EQUAL(SE_LOCK_IDLE, se_lock_on_release(s));
}

void test_lock_tx_gate(void)
{
    TEST_ASSERT_TRUE(se_lock_tx_allowed(SE_LOCK_IDLE, false));
    TEST_ASSERT_TRUE(se_lock_tx_allowed(SE_LOCK_COMMITTED, true));
    TEST_ASSERT_FALSE(se_lock_tx_allowed(SE_LOCK_COMMITTED, false));
    TEST_ASSERT_FALSE(se_lock_tx_allowed(SE_LOCK_FAILED, false));
    TEST_ASSERT_FALSE(se_lock_tx_allowed(SE_LOCK_RECOVERY, false));
    TEST_ASSERT_TRUE(se_lock_tx_allowed(SE_LOCK_RECOVERY, true));
}

void test_lock_http_gate(void)
{
    /* nothing held: everything passes */
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_IDLE, false, "/api/reboot", false));

    /* held: reads pass, changes don't */
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_COMMITTED, true, "/api/status", true));
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/api/system/reboot", true));
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/api/ota/upload", true));
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/settings/can_manager", true));

    /* the run's own controls pass */
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/api/scripts/stop", true));
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/api/scripts/answer", true));
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_FAILED, false, "/api/scripts/release", false));
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/api/scripts/stopper", true));

    /* a recovery run: only from failed/recovery, only when nothing runs */
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_COMMITTED, false, "/api/scripts/run", true));
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_RECOVERY, false, "/api/scripts/run", false));
    TEST_ASSERT_TRUE(se_lock_http_allowed(SE_LOCK_FAILED, false, "/api/scripts/run?x=1", false));
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_FAILED, false, "/api/scripts/run", true));
    TEST_ASSERT_FALSE(se_lock_http_allowed(SE_LOCK_FAILED, false, NULL, false));
}

/* ---- checksums ---------------------------------------------------------------- */

static const uint8_t CHECK[] = "123456789";

void test_crc_catalogue(void)
{
    uint32_t c = 0;
    se_crc_spec_t crc32 = { 32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF };
    se_crc_spec_t ccitt = { 16, 0x1021, 0xFFFF, false, false, 0 };
    se_crc_spec_t crc8 = { 8, 0x07, 0, false, false, 0 };
    se_crc_spec_t bad = { 0, 1, 0, false, false, 0 };

    TEST_ASSERT_TRUE(se_crc(&crc32, CHECK, 9, &c));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, c);
    TEST_ASSERT_TRUE(se_crc(&ccitt, CHECK, 9, &c));
    TEST_ASSERT_EQUAL_HEX32(0x29B1, c);
    TEST_ASSERT_TRUE(se_crc(&crc8, CHECK, 9, &c));
    TEST_ASSERT_EQUAL_HEX32(0xF4, c);
    TEST_ASSERT_FALSE(se_crc(&bad, CHECK, 9, &c));
}

void test_adler32_and_sum(void)
{
    /* zlib's check value for "Wikipedia" */
    const uint8_t w[] = "Wikipedia";

    TEST_ASSERT_EQUAL_HEX32(0x11E60398, se_adler32(1, w, 9));
    /* continuing gives the same as one pass */
    TEST_ASSERT_EQUAL_HEX32(0x11E60398, se_adler32(se_adler32(1, w, 4), w + 4, 5));

    const uint8_t d[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

    TEST_ASSERT_EQUAL_HEX32(15, se_word_sum(d, 5, 1, false));
    /* LE words 0x0201 + 0x0403 + 0x0005 */
    TEST_ASSERT_EQUAL_HEX32(0x0609, se_word_sum(d, 5, 2, false));
    /* BE words 0x0102 + 0x0304 + 0x0500 */
    TEST_ASSERT_EQUAL_HEX32(0x0906, se_word_sum(d, 5, 2, true));
    TEST_ASSERT_EQUAL_HEX32(0x04030201 + 0x05, se_word_sum(d, 5, 4, false));
    TEST_ASSERT_EQUAL_HEX32(0, se_word_sum(d, 5, 3, false));
}
