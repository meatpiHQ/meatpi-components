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
 * @brief Chip firmware update: a distinct EXCLUSIVE state: fan-out pauses,
 *        send() rejects, this module owns the wire with byte-level reads.
 *
 * The wire protocol is the VERIFIED legacy flow (elm327.c, tested in
 * production: do not improvise):
 *   1. VTVERS            -> "MIC3624 ... Vx.y.z" (identity + version check)
 *   2. VTDLMIC3422       -> OK           (enter download mode)
 *   3. VTDLDT<line>      -> OK           (one vendor hex record per line)
 *   4. line "FFF1..."    -> end marker: stop sending records
 *   5. VTDLED            -> OK           (finalize; retried 3x, 2 s settle)
 *   6. hardware reset pulse (GPIO41), rewake, re-probe
 *
 * Update responses DO end with the '>' prompt (bench-verified 2026-07-03):
 * collect until '>' (that is the chip's "ready for the next command" signal)
 * then classify the text: "OK"/"MIC3624" = accepted, "?" = rejected. Returning
 * early on "OK\r" without waiting for '>' makes the next record race the
 * chip's mode switch and get dropped (it answers a bare "\r>"). VTDLED is the
 * one exception: legacy exits early on OK+CR since the chip is about to reset.
 *
 * Issue #1023 (2026-10-28): the gate no longer takes "no prompt" for "no
 * chip". It probes VTVERS at the baud the UART runs at and at the other one
 * (a chip stuck in download mode sits at 2 Mbaud while a failed bring-up
 * leaves the UART at 115200), logs what came back, and, like legacy v4.52p,
 * still tries the download-mode entry when bytes arrived without a prompt:
 * only a wire silent at both bauds aborts (`force` overrides even that).
 * The packaged image is V2.3.24 and a chip at V2.3.22 is left alone (the
 * v4.52p policy): v6 used to downgrade a chip v4.52p had taken to V2.3.24.
 * The update runs as a job for the console and HTTP, reports its progress,
 * and the sleep entry waits for it.
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

/* keep in lockstep with EMBED_TXTFILES in CMakeLists.txt and the accepted
 * list in obd_chip_parse.c (obd_fw_version_is_current) */
#define OBD_FW_BUILTIN_VERSION "V2.3.24"

/* ---- wire diagnostics (issue #1023) ------------------------------------------ */

/* what the last fw_command saw, prompt or not */
static size_t  s_rx_total;
static uint8_t s_rx_head[12];
static size_t  s_rx_head_len;

static void rx_reset(void)
{
    s_rx_total = 0;
    s_rx_head_len = 0;
}

static void rx_note(uint8_t byte)
{
    if (s_rx_head_len < sizeof(s_rx_head))
    {
        s_rx_head[s_rx_head_len++] = byte;
    }

    s_rx_total++;
}

/** One W line about a command that got no prompt: silent, or N bytes with
 *  the first of them in hex (a chip at 2 Mbaud with its echo on answers the
 *  garbled 115200 bytes with hundreds of bytes of echo read at the wrong
 *  speed; with the echo off it answers nothing: silence alone does not
 *  tell a dead wire from a chip at another speed). */
static void wire_note(const char *what, int baud)
{
    if (s_rx_total == 0)
    {
        ESP_LOGW(TAG, "%s at %d baud: nothing received", what, baud);
        return;
    }

    char hex[3 * sizeof(s_rx_head) + 1];
    size_t n = 0;

    for (size_t i = 0; i < s_rx_head_len && n < sizeof(hex) - 3; i++)
    {
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%s%02X",
                              (i > 0) ? " " : "", s_rx_head[i]);
    }

    ESP_LOGW(TAG, "%s at %d baud: %u byte(s) but no prompt (%s%s)", what,
             baud, (unsigned)s_rx_total, hex,
             (s_rx_total > s_rx_head_len) ? " ..." : "");
}

static int other_baud(int baud)
{
    return (baud == OBD_CHIP_BAUD) ? OBD_CHIP_FALLBACK_BAUD : OBD_CHIP_BAUD;
}

/* ---- status (GET /api/obd_chip, the obd_chip command, the sleep entry) ----- */

static volatile bool        s_running;
static bool                 s_last_flashed;
static obd_chip_fw_status_t s_status;
static char                 s_source[72];

static void status_begin(const char *src, uint32_t total)
{
    strlcpy(s_source, src, sizeof(s_source));
    s_status.source = s_source;
    s_status.lines = 0;
    s_status.total = total;
    s_status.running = true;
    s_running = true;
}

/** Every exit of the engine after the claim: book the result, release. */
static esp_err_t status_end(esp_err_t err, bool flashed)
{
    s_status.running = false;
    s_status.done_once = true;
    s_status.last_err = err;
    s_last_flashed = flashed;
    s_running = false;
    obd_core_release();
    return err;
}

bool obd_chip_update_in_progress(void)
{
    return s_running;
}

bool obd_fw_last_flashed(void)
{
    return s_last_flashed;
}

/* the chip's VTVERS as last read by the bring-up or the update gate: the
 * page shows it without a wire request (GET /api/obd_chip never touches
 * the wire); "" until a chip answered */
static char s_chip_version[32];

void obd_chip_version_note(const char *vtvers)
{
    if (vtvers == NULL || strstr(vtvers, FW_TARGET_ID) == NULL)
    {
        return;
    }

    size_t n = 0;

    for (const char *p = vtvers; *p != '\0' && n + 1 < sizeof(s_chip_version); p++)
    {
        if (*p == '\r' || *p == '\n' || *p == '>')
        {
            break;
        }

        s_chip_version[n++] = *p;
    }

    s_chip_version[n] = '\0';
}

void obd_chip_fw_status(obd_chip_fw_status_t *out)
{
    *out = s_status;
    out->chip = s_chip_version;

    if (out->source == NULL)
    {
        out->source = "";
    }
}

/* ---- the wire ------------------------------------------------------------------ */

/**
 * Send one update command and match its reply (exact legacy semantics): skip
 * the echo, collect until the '>' prompt (VTDLED: until OK+CR), then classify.
 * Returns ESP_ERR_NOT_FOUND on "?" (rejected), ESP_FAIL on unexpected text,
 * ESP_ERR_TIMEOUT when no prompt arrived (@p resp holds what did).
 */
static esp_err_t fw_command(const char *cmd, char *resp, size_t resp_len,
                            uint32_t timeout_ms)
{
    size_t cmd_len = strlen(cmd);
    bool is_vtdled = (strstr(cmd, "VTDLED") != NULL);

    obd_uart_flush_input();
    rx_reset();

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

        rx_note(byte);

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

            echo_skipped = true; /* no echo: chip already answered */
        }

        /* VTDLED: the chip resets right after, don't insist on a prompt */
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

/** Records in an image: the progress denominator (one pass over ~490 KB). */
static uint32_t fw_count_records(const char *buf, size_t len)
{
    uint32_t n = 0;

    for (size_t i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
        {
            n++;
        }
    }

    return n;
}

/**
 * Step 1: identity + version gate. Probes VTVERS at the baud the UART runs
 * at, then at the other one. Writes @p resp (the answer, prompt or not),
 * @p prompt (a '>' arrived: the UART stays at that baud), @p normal (the
 * chip named itself) and @p vt_err (the VTVERS verdict). ESP_ERR_TIMEOUT
 * only for a wire silent at both bauds (unless forced), the UART back
 * where it was; ESP_OK otherwise: the caller goes on.
 */
static esp_err_t fw_gate(bool force, char *resp, size_t resp_len,
                         bool *prompt, bool *normal, esp_err_t *vt_err)
{
    const int first = obd_uart_get_baud();
    const int bauds[2] = { first, other_baud(first) };
    size_t bytes = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;

    *prompt = false;
    *normal = false;

    for (size_t i = 0; i < 2; i++)
    {
        if (i > 0)
        {
            obd_uart_set_baud(bauds[i]);
        }

        err = fw_command("VTVERS\r", resp, resp_len, FW_TIMEOUT_MS);

        if (err != ESP_ERR_TIMEOUT)
        {
            *prompt = true; /* the chip talks at this baud: stay here */
            break;
        }

        wire_note("VTVERS", bauds[i]);
        bytes += s_rx_total;
    }

    *vt_err = err;

    if (*prompt)
    {
        *normal = (err == ESP_OK && strstr(resp, FW_TARGET_ID) != NULL);
        return ESP_OK;
    }

    if (bytes == 0 && !force)
    {
        ESP_LOGW(TAG, "no answer on the wire at %d or %d baud (VTVERS "
                 "timeout, nothing received), not flashing; use force to "
                 "override", bauds[0], bauds[1]);
        obd_uart_set_baud(first);
        return ESP_ERR_TIMEOUT;
    }

    /* bytes without a prompt (a bootloader, a reboot loop) or forced:
       legacy v4.52p went on to the download-mode entry here */
    ESP_LOGW(TAG, "chip answers without a prompt%s: trying the download-mode "
             "entry to recover it", force ? " (forced)" : "");
    return ESP_OK;
}

/** Step 2: VTDLMIC3422 must answer OK. When the gate saw no prompt the
 *  chip's baud is unknown: the other one gets a try too. */
static esp_err_t fw_enter_download(bool prompt_seen, char *resp,
                                   size_t resp_len)
{
    esp_err_t err = fw_command("VTDLMIC3422\r", resp, resp_len, FW_TIMEOUT_MS);

    if (err != ESP_OK && !prompt_seen)
    {
        int here = obd_uart_get_baud();

        wire_note("VTDLMIC3422", here);
        obd_uart_set_baud(other_baud(here));
        err = fw_command("VTDLMIC3422\r", resp, resp_len, FW_TIMEOUT_MS);

        if (err != ESP_OK)
        {
            wire_note("VTDLMIC3422", other_baud(here));
        }
    }

    return err;
}

/** The engine, source-agnostic: @p fw_buf holds the whole vendor image
 *  text, @p ver its "V2.3.24"-style version (the file's name, or the
 *  packaged constant), @p packaged selects the current-version policy
 *  (a file skips only a chip at exactly its version), @p src a label
 *  for the logs. */
static esp_err_t fw_update_run(const char *fw_buf, size_t fw_len,
                               const char *ver, bool packaged, bool force,
                               const char *src)
{
    static char line[FW_LINE_MAX];
    static char resp[FW_RESP_MAX];
    static char cmd[FW_LINE_MAX + 10];

    /* async bring-up: EXCLUSIVE would fight the bare-UART negotiation */
    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* EXCLUSIVE: fan-out pauses, send() rejects, we own the wire now */
    esp_err_t err = obd_core_claim(3 /* CLAIM_EXCLUSIVE */, 5000);

    if (err != ESP_OK)
    {
        return err;
    }

    /* let an in-flight RX-task read cycle finish before we take the UART */
    vTaskDelay(pdMS_TO_TICKS(100));

    const int baud_found = obd_uart_get_baud();

    status_begin(src, fw_count_records(fw_buf, fw_len));

    /* 1. identity + version gate. Legacy semantics: '?' means the chip is
       not in its normal state (typically stuck in download mode after an
       interrupted update) and COMPLETING a download is the recovery path,
       so proceed. Only a healthy chip at a current version skips. */
    bool prompt;
    bool normal;
    esp_err_t vt_err;

    err = fw_gate(force, resp, sizeof(resp), &prompt, &normal, &vt_err);

    if (err != ESP_OK)
    {
        return status_end(err, false);
    }

    if (normal)
    {
        obd_chip_version_note(resp);
        ESP_LOGI(TAG, "chip reports: %s", resp);

        bool current = packaged ? obd_fw_version_is_current(resp)
                                : (ver[0] != '\0' && strstr(resp, ver) != NULL);

        if (!force && current)
        {
            if (packaged)
            {
                ESP_LOGI(TAG, "chip firmware is current (packaged %s; %s "
                         "are current): nothing to do", ver,
                         obd_fw_current_versions());
            }
            else
            {
                ESP_LOGI(TAG, "already at %s: nothing to do", ver);
            }

            return status_end(ESP_OK, false);
        }
    }
    else if (prompt)
    {
        ESP_LOGW(TAG, "chip not in normal state (VTVERS: %s, '%s'), "
                 "updating anyway to recover it", esp_err_to_name(vt_err),
                 resp);
    }

    /* 2. enter download mode */
    ESP_LOGW(TAG, "starting chip firmware update from %s (%u bytes, %lu "
             "records)", src, (unsigned)fw_len,
             (unsigned long)s_status.total);
    err = fw_enter_download(prompt, resp, sizeof(resp));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "VTDLMIC3422 rejected (%s, '%s')", esp_err_to_name(err),
                 resp);
        obd_uart_set_baud(baud_found);
        return status_end(err, false);
    }

    /* 3./4. stream records until the FFF1 end marker */
    obd_fw_iter_t it;
    uint32_t line_no = 0;
    bool complete = false;

    obd_fw_iter_init(&it, fw_buf, fw_len);

    while (obd_fw_iter_next(&it, line, sizeof(line)) > 0)
    {
        line_no++;
        s_status.lines = line_no;

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

    /* 6. hardware reset + settle, regardless of outcome. The chip boots at
       its stored power-on baud; the caller brings it up again. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    obd_pin_reset_pulse();
    obd_uart_flush_input();
    vTaskDelay(pdMS_TO_TICKS(2000));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "chip firmware update complete (%lu records)",
                 (unsigned long)line_no);
    }

    return status_end(err, err == ESP_OK);
}

esp_err_t obd_chip_firmware_update(const char *fs_path, bool force)
{
    /* whole file into PSRAM: vendor images are ~500 KB of text */
    static char *s_fw_buf;
    static size_t s_fw_cap;

    size_t fw_size = 0;
    esp_err_t err = filesystem_size(fs_path, &fw_size);

    if (err != ESP_OK || fw_size == 0)
    {
        ESP_LOGE(TAG, "fw file '%s' unavailable (%s)", fs_path,
                 esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    if (s_fw_buf != NULL && s_fw_cap < fw_size + 1)
    {
        heap_caps_free(s_fw_buf); /* a bigger image than the last one */
        s_fw_buf = NULL;
    }

    if (s_fw_buf == NULL)
    {
        s_fw_buf = heap_caps_malloc(fw_size + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_fw_cap = fw_size + 1;
    }

    if (s_fw_buf == NULL)
    {
        s_fw_cap = 0;
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

    return fw_update_run(s_fw_buf, got, ver, false, force, fs_path);
}

/* The packaged image (EMBED_TXTFILES appends a NUL). Bumping the
 * packaged version = new file in CMakeLists + this constant + the
 * current list in obd_chip_parse.c. */
extern const char _binary_V2_3_24_txt_start[];
extern const char _binary_V2_3_24_txt_end[];

const char *obd_chip_builtin_fw_version(void)
{
    return OBD_FW_BUILTIN_VERSION;
}

esp_err_t obd_chip_firmware_update_builtin(bool force)
{
    size_t len = (size_t)(_binary_V2_3_24_txt_end -
                          _binary_V2_3_24_txt_start);

    return fw_update_run(_binary_V2_3_24_txt_start, len,
                         OBD_FW_BUILTIN_VERSION, true, force, "builtin "
                         OBD_FW_BUILTIN_VERSION);
}

