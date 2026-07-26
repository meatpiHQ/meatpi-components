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

/**
 * @file obd_chip_fw.c
 * @brief Chip firmware update — a distinct EXCLUSIVE state: fan-out pauses,
 *        send() rejects, this module owns the wire with byte-level reads.
 *
 * The wire protocol is the VERIFIED legacy flow (elm327.c, tested in
 * production — do not improvise):
 *   1. VTVERS            -> "MIC3624 ... Vx.y.z" (identity + version check)
 *   2. VTDLMIC3422       -> OK           (enter download mode)
 *   3. VTDLDT<line>      -> OK           (one vendor hex record per line)
 *   4. line "FFF1..."    -> end marker: stop sending records
 *   5. VTDLED            -> OK           (finalize; retried 3x, 2 s settle)
 *   6. hardware reset pulse (GPIO41), rewake, re-probe
 *
 * Update responses DO end with the '>' prompt (bench-verified 2026-07-03):
 * collect until '>' — that is the chip's "ready for the next command" signal —
 * then classify the text: "OK"/"MIC3624" = accepted, "?" = rejected. Returning
 * early on "OK\r" without waiting for '>' makes the next record race the
 * chip's mode switch and get dropped (it answers a bare "\r>"). VTDLED is the
 * one exception: legacy exits early on OK+CR since the chip is about to reset.
 */
#include "obd_chip.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "filesystem.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip"; /* one TAG per component (§10) */

#define FW_LINE_MAX    256
#define FW_RESP_MAX    256
#define FW_TIMEOUT_MS  5000
#define FW_TARGET_ID   "MIC3624"

/* keep in lockstep with EMBED_TXTFILES in CMakeLists.txt */
#define OBD_FW_BUILTIN_VERSION "V2.3.22"

/**
 * Send one update command and match its reply (exact legacy semantics): skip
 * the echo, collect until the '>' prompt (VTDLED: until OK+CR), then classify.
 * Returns ESP_ERR_NOT_FOUND on "?" (rejected), ESP_FAIL on unexpected text.
 */
static esp_err_t fw_command(const char *cmd, char *resp, size_t resp_len,
                            uint32_t timeout_ms)
{
    size_t cmd_len = strlen(cmd);
    bool is_vtdled = (strstr(cmd, "VTDLED") != NULL);

    obd_uart_flush_input();

    esp_err_t err = obd_uart_write((const uint8_t *)cmd, cmd_len);

    if (err != ESP_OK)
    {
        return err;
    }

    size_t len = 0;
    bool echo_skipped = false;
    bool prompt_seen = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    resp[0] = '\0';

    while (xTaskGetTickCount() < deadline)
    {
        uint8_t byte;

        if (obd_uart_read(&byte, 1, 50) != 1)
        {
            continue;
        }

        if (len < resp_len - 1)
        {
            resp[len++] = (char)byte;
            resp[len] = '\0';
        }

        /* drop the echo of our own command once it is fully received */
        if (!echo_skipped)
        {
            size_t cmp = (len < cmd_len) ? len : cmd_len;

            if (strncmp(resp, cmd, cmp) == 0)
            {
                if (len >= cmd_len)
                {
                    len = 0;
                    resp[0] = '\0';
                    echo_skipped = true;
                }

                continue;
            }

            echo_skipped = true; /* no echo — chip already answered */
        }

        /* VTDLED: the chip resets right after — don't insist on a prompt */
        if (is_vtdled && byte == '\r' && strstr(resp, "OK") != NULL)
        {
            return ESP_OK;
        }

        if (byte == '>')
        {
            prompt_seen = true; /* chip is ready for the next command */
            break;
        }
    }

    if (!prompt_seen)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (strstr(resp, "OK") != NULL || strstr(resp, FW_TARGET_ID) != NULL)
    {
        return ESP_OK;
    }

    if (strchr(resp, '?') != NULL)
    {
        return ESP_ERR_NOT_FOUND; /* command rejected */
    }

    return ESP_FAIL; /* prompt with unexpected text */
}

esp_err_t obd_chip_get_fw_version_raw(char *buf, size_t buf_len)
{
    return fw_command("VTVERS\r", buf, buf_len, FW_TIMEOUT_MS);
}

/** The engine, source-agnostic: @p fw_buf holds the whole vendor image
 *  text, @p ver the "V2.3.22"-style gate string (may be empty = no
 *  version skip), @p src a label for the logs. */
static esp_err_t fw_update_run(const char *fw_buf, size_t fw_len,
                               const char *ver, bool force, const char *src)
{
    static char line[FW_LINE_MAX];
    static char resp[FW_RESP_MAX];
    static char cmd[FW_LINE_MAX + 10];

    /* async bring-up: EXCLUSIVE would fight the bare-UART negotiation */
    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* EXCLUSIVE: fan-out pauses, send() rejects — we own the wire now */
    esp_err_t err = obd_core_claim(3 /* CLAIM_EXCLUSIVE */, 5000);

    if (err != ESP_OK)
    {
        return err;
    }

    /* let an in-flight RX-task read cycle finish before we take the UART */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 1. identity + version gate. Legacy semantics: '?' means the chip is
       not in its normal state — typically stuck in download mode after an
       interrupted update — and COMPLETING a download is the recovery path,
       so proceed. Only a healthy chip already at the target version skips.
       Stricter than legacy in ONE case: a VTVERS TIMEOUT (dead wire — no
       chip, no download-stuck bootloader) aborts instead of streaming
       ~3.6k records into 5 s timeouts; `force` overrides. */
    err = fw_command("VTVERS\r", resp, sizeof(resp), FW_TIMEOUT_MS);

    if (err == ESP_ERR_TIMEOUT && !force)
    {
        ESP_LOGW(TAG, "no answer on the wire (VTVERS timeout) — not "
                 "flashing; use force to override");
        obd_core_release();
        return ESP_ERR_TIMEOUT;
    }

    bool normal_state = (err == ESP_OK && strstr(resp, FW_TARGET_ID) != NULL);

    if (normal_state)
    {
        ESP_LOGI(TAG, "chip reports: %s", resp);

        if (!force && ver[0] != '\0' && strstr(resp, ver) != NULL)
        {
            ESP_LOGI(TAG, "already at %s — nothing to do", ver);
            obd_core_release();
            return ESP_OK;
        }
    }
    else
    {
        ESP_LOGW(TAG, "chip not in normal state (VTVERS: %s, '%s') — "
                 "updating anyway to recover it", esp_err_to_name(err), resp);
    }

    /* 2. enter download mode */
    ESP_LOGW(TAG, "starting chip firmware update from %s (%u bytes)",
             src, (unsigned)fw_len);
    err = fw_command("VTDLMIC3422\r", resp, sizeof(resp), FW_TIMEOUT_MS);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "VTDLMIC3422 rejected: '%s'", resp);
        obd_core_release();
        return err;
    }

    /* 3./4. stream records until the FFF1 end marker */
    obd_fw_iter_t it;
    uint32_t line_no = 0;
    bool complete = false;

    obd_fw_iter_init(&it, fw_buf, fw_len);

    while (obd_fw_iter_next(&it, line, sizeof(line)) > 0)
    {
        line_no++;

        if (obd_fw_line_is_end_marker(line))
        {
            complete = true;
            break;
        }

        snprintf(cmd, sizeof(cmd), "VTDLDT%s\r", line);
        err = fw_command(cmd, resp, sizeof(resp), FW_TIMEOUT_MS);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "record %lu rejected ('%s')",
                     (unsigned long)line_no, resp);
            break;
        }

        if ((line_no % 500) == 0)
        {
            ESP_LOGI(TAG, "update progress: %lu lines",
                     (unsigned long)line_no);
        }

        vTaskDelay(1); /* legacy pacing */
    }

    /* 5. finalize (legacy: 2 s settle, then up to 3 attempts) */
    if (err == ESP_OK && complete)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        err = ESP_FAIL;

        for (int attempt = 1; attempt <= 3; attempt++)
        {
            esp_err_t r = fw_command("VTDLED\r", resp, sizeof(resp),
                                     FW_TIMEOUT_MS);

            if (r == ESP_OK)
            {
                err = ESP_OK;
                break;
            }

            err = r;
            ESP_LOGW(TAG, "VTDLED attempt %d failed (%s)", attempt,
                     esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    else if (err == ESP_OK)
    {
        err = ESP_ERR_INVALID_RESPONSE; /* file had no FFF1 end marker */
    }

    /* 6. hardware reset + settle, regardless of outcome */
    vTaskDelay(pdMS_TO_TICKS(2000));
    obd_pin_reset_pulse();
    obd_uart_flush_input();
    vTaskDelay(pdMS_TO_TICKS(2000));

    obd_core_release();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "chip firmware update complete (%lu records)",
                 (unsigned long)line_no);
    }

    return err;
}

esp_err_t obd_chip_firmware_update(const char *fs_path, bool force)
{
    /* whole file into PSRAM: vendor images are ~500 KB of text */
    static char *s_fw_buf;

    size_t fw_size = 0;
    esp_err_t err = filesystem_size(fs_path, &fw_size);

    if (err != ESP_OK || fw_size == 0)
    {
        ESP_LOGE(TAG, "fw file '%s' unavailable (%s)", fs_path,
                 esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    if (s_fw_buf == NULL)
    {
        s_fw_buf = heap_caps_malloc(fw_size + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (s_fw_buf == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;

    err = filesystem_read(fs_path, s_fw_buf, fw_size + 1, &got);

    if (err != ESP_OK)
    {
        return err;
    }

    /* version gate string from the filename (the established contract) */
    char ver[16] = "";
    const char *v = strstr(fs_path, "V2.");

    if (v != NULL)
    {
        snprintf(ver, sizeof(ver), "%.7s", v);
    }

    return fw_update_run(s_fw_buf, got, ver, force, fs_path);
}

/* The packaged image (EMBED_TXTFILES appends a NUL). Bumping the
 * packaged version = new file in CMakeLists + this constant. */
extern const char _binary_V2_3_22_txt_start[];
extern const char _binary_V2_3_22_txt_end[];

const char *obd_chip_builtin_fw_version(void)
{
    return OBD_FW_BUILTIN_VERSION;
}

esp_err_t obd_chip_firmware_update_builtin(bool force)
{
    size_t len = (size_t)(_binary_V2_3_22_txt_end -
                          _binary_V2_3_22_txt_start);

    return fw_update_run(_binary_V2_3_22_txt_start, len,
                         OBD_FW_BUILTIN_VERSION, force, "builtin "
                         OBD_FW_BUILTIN_VERSION);
}
