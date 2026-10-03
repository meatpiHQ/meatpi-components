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
 * @file obd_chip.c
 * @brief Lifecycle, subscriber registry (fan-out), and the claim model.
 *
 * Fan-out contract (README): every subscriber gets every chunk; a full queue
 * drops that subscriber's copy (counted, rate-limited WARN) and never stalls
 * the RX task or other subscribers.
 */
#include "obd_chip.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "log_manager.h"
#include "obd_gate.h"

#include "obd_chip_private.h"

static const char *TAG = "obd_chip";

/* obd_gate owner identity for the MIC chip (address only; shared with
 * obd_chip_cmd.c via obd_chip_private.h) */
const char obd_chip_gate_owner[1];

#define OBD_MAX_SUBSCRIBERS 8

typedef struct
{
    QueueHandle_t q;
    const char   *name;
    uint32_t      dropped;
    uint32_t      dropped_logged;
} obd_sub_t;

typedef enum
{
    CLAIM_NONE = 0,
    CLAIM_COMMAND,
    CLAIM_MONITOR,
    CLAIM_EXCLUSIVE,
} claim_state_t;

/* external-client activity clock (obd_chip_client_touch): the last time a
 * bridge (TCP/BLE/USB/WS app) wrote to the chip; 0 = never. Read by
 * autopid to yield the chip while an app drives it (legacy
 * DEV_AUTOPID_ELM327_APP_BIT parity, bench 2026-09-08). */
static volatile int64_t s_client_touch_us;

/* RX fan-out accounting (GET /api/obd_chip): what the chip printed vs
 * what each subscriber managed to take - the first hop of any "missed
 * frames" question (ATMA floods, 8 KB VT responses). RX-task-only writes. */
static uint32_t s_rx_bytes;
static uint32_t s_rx_chunks;
static uint16_t s_rx_max_chunk;

/* EEPROM guard accounting (obd_chip_guard.h) */
static uint32_t s_guard_rewrites;
static uint32_t s_guard_blocked;

/* multi-command transaction hold (obd_chip_txn_begin): the task that owns
 * the COMMAND claim across several requests. A request from THIS task
 * neither claims nor releases (the transaction owns the claim). s_lock-
 * protected. s_txn_start_us arms a fail-open steal so a transaction whose
 * end() never ran (e.g. an httpd worker recycled mid-request) cannot brick
 * the chip for other requesters — mirrors obd_gate's max-hold expiry. */
static TaskHandle_t s_txn_task;
static int64_t      s_txn_start_us;
#define OBD_TXN_MAX_US (12 * 1000 * 1000) /* > worst UDS transaction */

/* registry + state: PSRAM .bss (§2) */
static obd_sub_t s_subs[OBD_MAX_SUBSCRIBERS] EXT_RAM_BSS_ATTR;
static size_t s_sub_count;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* internal: spinlock */
static volatile claim_state_t s_claim = CLAIM_NONE;

static bool s_inited;
static bool s_started;
static int  s_active_baud;

/* async bring-up (2026-07-26, meatpi: get the ~2 s ATZ/baud negotiation
 * off the boot path): start() launches a one-shot task; the DONE bit
 * flips when the sequence finishes (chip up OR given up). Wire-touching
 * APIs gate on DONE so nothing interleaves with the bare-UART probes. */
#define BRINGUP_DONE_BIT BIT0

static EventGroupHandle_t s_bringup_evt;
static StaticEventGroup_t s_bringup_evt_buf; /* internal: FreeRTOS object */
static volatile bool s_bringup_active;

/* ---- bring-up gate -------------------------------------------------------------- */

bool obd_core_bringup_wait(uint32_t timeout_ms)
{
    if (!s_bringup_active || s_bringup_evt == NULL)
    {
        return true; /* finished, or start() never launched (degraded) */
    }

    EventBits_t bits = xEventGroupWaitBits(s_bringup_evt, BRINGUP_DONE_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));

    return (bits & BRINGUP_DONE_BIT) != 0;
}

/* ---- fan-out ------------------------------------------------------------------ */

void obd_core_fanout(const uint8_t *data, size_t len)
{
    obd_chunk_t chunk;

    /* obd_gate: the chip's conversation window ends at the '>' prompt
       (line start only — response DATA may contain '>' mid-line, see
       obd_chip_parse.c). Release is idempotent; the max-hold expiry
       covers monitor-class commands that never print a prompt. */
    if (obd_gate_enabled())
    {
        static uint8_t s_prev = '\n'; /* RX-task-only state */

        for (size_t i = 0; i < len; i++)
        {
            if (data[i] == '>' && (s_prev == '\r' || s_prev == '\n'))
            {
                obd_gate_release(obd_chip_gate_owner);
            }

            s_prev = data[i];
        }
    }

    chunk.len = (uint16_t)((len > OBD_CHIP_CHUNK_SIZE) ? OBD_CHIP_CHUNK_SIZE
                                                       : len);
    memcpy(chunk.data, data, chunk.len);

    s_rx_bytes += chunk.len;
    s_rx_chunks++;

    if (chunk.len > s_rx_max_chunk)
    {
        s_rx_max_chunk = chunk.len;
    }

    for (size_t i = 0; i < s_sub_count; i++)
    {
        if (s_subs[i].q == NULL)
        {
            continue;
        }

        if (xQueueSend(s_subs[i].q, &chunk, 0) != pdTRUE)
        {
            s_subs[i].dropped++;

            /* rate limit: one WARN per 100 drops per subscriber (§10) */
            if (s_subs[i].dropped - s_subs[i].dropped_logged >= 100)
            {
                s_subs[i].dropped_logged = s_subs[i].dropped;
                ESP_LOGW(TAG, "subscriber '%s' dropped %lu chunks",
                         s_subs[i].name, (unsigned long)s_subs[i].dropped);
            }
        }
    }
}

esp_err_t obd_chip_subscribe(QueueHandle_t q, const char *name)
{
    if (q == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NO_MEM;

    portENTER_CRITICAL(&s_lock);

    for (size_t i = 0; i < OBD_MAX_SUBSCRIBERS; i++)
    {
        if (s_subs[i].q == NULL)
        {
            s_subs[i].q = q;
            s_subs[i].name = (name != NULL) ? name : "?";
            s_subs[i].dropped = 0;
            s_subs[i].dropped_logged = 0;

            if (i >= s_sub_count)
            {
                s_sub_count = i + 1;
            }

            err = ESP_OK;
            break;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t obd_chip_unsubscribe(QueueHandle_t q)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_lock);

    for (size_t i = 0; i < s_sub_count; i++)
    {
        if (s_subs[i].q == q)
        {
            s_subs[i].q = NULL;
            err = ESP_OK;
            break;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    return err;
}

uint32_t obd_chip_dropped(QueueHandle_t q)
{
    for (size_t i = 0; i < s_sub_count; i++)
    {
        if (s_subs[i].q == q)
        {
            return s_subs[i].dropped;
        }
    }

    return 0;
}

/* ---- claims -------------------------------------------------------------------- */

esp_err_t obd_core_claim(int type, uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    while (true)
    {
        bool granted = false;
        bool monitor_blocks = false;

        portENTER_CRITICAL(&s_lock);

        /* a request from the transaction owner already holds the claim */
        if (type == CLAIM_COMMAND && s_txn_task != NULL &&
            s_txn_task == self)
        {
            granted = true;
        }
        /* a transaction whose end() never ran can't brick the chip: steal
           the COMMAND claim after the max hold (fail-open, like obd_gate) */
        else if (s_claim == CLAIM_COMMAND && s_txn_task != NULL &&
                 (esp_timer_get_time() - s_txn_start_us) > OBD_TXN_MAX_US)
        {
            s_txn_task = NULL;
            s_claim = (claim_state_t)type;
            granted = true;
        }
        else if (s_claim == CLAIM_NONE)
        {
            s_claim = (claim_state_t)type;
            granted = true;
        }
        else if (s_claim == CLAIM_MONITOR && type == CLAIM_COMMAND)
        {
            monitor_blocks = true;
        }

        portEXIT_CRITICAL(&s_lock);

        if (granted)
        {
            return ESP_OK;
        }

        /* v1 policy "manual": a held MONITOR is never auto-interrupted —
           commands fail fast so callers can surface "bus busy" (task §5) */
        if (monitor_blocks)
        {
            return ESP_ERR_INVALID_STATE;
        }

        if (xTaskGetTickCount() >= deadline)
        {
            return (s_claim == CLAIM_EXCLUSIVE) ? ESP_ERR_INVALID_STATE
                                                : ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void obd_core_release(void)
{
    portENTER_CRITICAL(&s_lock);

    /* a nested request inside a held transaction does NOT release — the
       transaction owns the claim until obd_chip_txn_end() */
    if (s_txn_task == NULL || s_txn_task != xTaskGetCurrentTaskHandle())
    {
        s_claim = CLAIM_NONE;
    }

    portEXIT_CRITICAL(&s_lock);
}

bool obd_core_exclusive_held(void)
{
    return s_claim == CLAIM_EXCLUSIVE;
}

esp_err_t obd_chip_claim(obd_claim_t type, TickType_t timeout)
{
    return obd_core_claim((int)type + 1 /* CLAIM_NONE offset */,
                          timeout * portTICK_PERIOD_MS);
}

esp_err_t obd_chip_release(void)
{
    obd_core_release();
    return ESP_OK;
}

esp_err_t obd_chip_txn_begin(TickType_t timeout)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool already;

    portENTER_CRITICAL(&s_lock);
    already = (s_txn_task == self);
    portEXIT_CRITICAL(&s_lock);

    if (already)
    {
        return ESP_ERR_INVALID_STATE; /* one transaction per task */
    }

    /* take the COMMAND claim as a normal requester (no txn owner yet), then
       become the owner: nested obd_chip_request() calls skip claim/release */
    esp_err_t err = obd_core_claim(CLAIM_COMMAND,
                                   timeout * portTICK_PERIOD_MS);

    if (err != ESP_OK)
    {
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    s_txn_task = self;
    s_txn_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void obd_chip_txn_end(void)
{
    portENTER_CRITICAL(&s_lock);

    if (s_txn_task == xTaskGetCurrentTaskHandle())
    {
        s_txn_task = NULL;
        s_claim = CLAIM_NONE;
    }

    portEXIT_CRITICAL(&s_lock);
}

void obd_core_guard_note(obd_guard_t v, const char *cmd, size_t len)
{
    char head[40];
    size_t n = 0;

    for (size_t i = 0; i < len && n < sizeof(head) - 1; i++)
    {
        char c = cmd[i];

        head[n++] = (c == '\r' || c == '\n') ? ' ' : c;
    }

    head[n] = '\0';

    if (v == OBD_GUARD_BLOCKED)
    {
        s_guard_blocked++;
        ESP_LOGW(TAG, "EEPROM guard refused '%s' (ATPP/ATSD/ATCV/STWBR "
                      "write the chip's EEPROM)", head);
    }
    else if (v == OBD_GUARD_REWRITTEN)
    {
        s_guard_rewrites++;
        ESP_LOGD(TAG, "EEPROM guard rewrote '%s' (ATSP->ATTP, ATM1->ATM0)",
                 head);
    }
}

bool obd_chip_is_monitor_cmd(const char *cmd)
{
    return obd_parse_is_monitor_cmd(cmd);
}

esp_err_t obd_chip_monitor_stop(void)
{
    /* SPACE, never CR: CR repeats the last command and can re-enter ATMA */
    static const uint8_t stop = ' ';

    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    return obd_uart_write(&stop, 1);
}

/* ---- TX ------------------------------------------------------------------------- */

esp_err_t obd_chip_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* a write mid-bring-up would interleave with the bare-UART probes —
       hold early boot-window clients until the chip is negotiated (a
       zero-cost flag test any time after) */
    if (!obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (obd_core_exclusive_held())
    {
        return ESP_ERR_INVALID_STATE; /* fw update owns the wire */
    }

    /* EEPROM guard (obd_chip_guard.h): apps re-send ATSP on every connect
       and a terminal user can type ATPP — rewrite the two with RAM twins
       in place, refuse the rest with the ELM "?" the sender expects, so
       the chip never sees an EEPROM write from a bridge. */
    uint8_t copy[256];
    obd_guard_t gv = obd_chip_guard_check((const char *)data, len);

    if (gv == OBD_GUARD_REWRITTEN && len > sizeof(copy))
    {
        gv = OBD_GUARD_BLOCKED; /* no scratch for a rewrite that long */
    }

    if (gv != OBD_GUARD_PASS)
    {
        obd_core_guard_note(gv, (const char *)data, len);
    }

    if (gv == OBD_GUARD_BLOCKED)
    {
        static const uint8_t refused[] = "?\r\r>";

        obd_core_fanout(refused, sizeof(refused) - 1);
        return ESP_OK;
    }

    if (gv == OBD_GUARD_REWRITTEN)
    {
        memcpy(copy, data, len);
        (void)obd_chip_guard_cmd((char *)copy, len);
        data = copy;
    }

    /* obd_gate: a CR submits a command to the chip — that opens a bus
       conversation, so serialize against the ESP-side ELM engines. May
       block up to OBD_GATE_WAIT_MS; released when the RX fan-out sees
       the '>' prompt (or by the max-hold failsafe). */
    if (memchr(data, '\r', len) != NULL)
    {
        (void)obd_gate_acquire(obd_chip_gate_owner, OBD_GATE_WAIT_MS);
    }

    return obd_uart_write(data, len);
}

/* ---- observability (GET /api/obd_chip) ----------------------------------------- */

esp_err_t obd_chip_get_stats(obd_chip_stats_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    static const char *CLAIMS[] = { "none", "command", "monitor",
                                    "exclusive" };

    memset(out, 0, sizeof(*out));
    out->ready = obd_chip_ready();
    out->claim = CLAIMS[(s_claim <= CLAIM_EXCLUSIVE) ? s_claim : 0];
    out->rx_bytes = s_rx_bytes;
    out->rx_chunks = s_rx_chunks;
    out->rx_max_chunk = s_rx_max_chunk;
    out->client_idle_ms = obd_chip_client_idle_ms();
    out->guard_rewrites = s_guard_rewrites;
    out->guard_blocked = s_guard_blocked;
    out->protocol_saves = obd_cmd_protocol_saves();
    obd_uart_get_stats(&out->tx_bytes, &out->rx_overflows,
                       &out->rx_buffered);
    return ESP_OK;
}

size_t obd_chip_get_subscribers(obd_chip_sub_stats_t *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < s_sub_count && n < max; i++)
    {
        QueueHandle_t q = s_subs[i].q;

        if (q == NULL)
        {
            continue;
        }

        out[n].name = s_subs[i].name;
        out[n].dropped = s_subs[i].dropped;
        out[n].queued = (uint32_t)uxQueueMessagesWaiting(q);
        out[n].depth = out[n].queued + (uint32_t)uxQueueSpacesAvailable(q);
        n++;
    }

    return n;
}

/* ---- external-client activity clock ------------------------------------------- */

void obd_chip_client_touch(void)
{
    s_client_touch_us = esp_timer_get_time();
}

uint32_t obd_chip_client_idle_ms(void)
{
    int64_t t = s_client_touch_us;

    if (t == 0)
    {
        return UINT32_MAX; /* never */
    }

    int64_t idle = (esp_timer_get_time() - t) / 1000;

    return (idle < 0) ? 0 : (idle > UINT32_MAX) ? UINT32_MAX
                                                : (uint32_t)idle;
}

/* ---- chip management ------------------------------------------------------------- */

esp_err_t obd_chip_sleep(bool sleep)
{
    if (sleep)
    {
        /* don't yank the chip asleep mid-bring-up (sleep entry already
           takes seconds; after DONE this is a flag test). Proceed on a
           wedged bring-up — sleeping is the stronger intent. */
        (void)obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS);
        obd_pin_sleep();
    }
    else
    {
        obd_pin_wake();
    }

    return ESP_OK;
}

esp_err_t obd_chip_hard_reset(void)
{
    (void)obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS);
    obd_pin_reset_pulse();
    return ESP_OK;
}

bool obd_chip_status_ok(void)
{
    return obd_pin_ready();
}

/* ---- lifecycle -------------------------------------------------------------------- */

esp_err_t obd_chip_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC = { "obd_chip", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC); /* per-TAG level control (§9.2) */

    /* first: the bring-up gate must exist even after a partial init
       (start() can still be reached — §4.3 degrade paths) */
    s_bringup_evt = xEventGroupCreateStatic(&s_bringup_evt_buf);

    esp_err_t err = obd_settings_register();

    if (err != ESP_OK)
    {
        return err;
    }

    obd_pins_init();
    err = obd_uart_init(115200); /* real baud negotiated in start() */

    if (err != ESP_OK)
    {
        return err;
    }

    err = obd_cmd_engine_init(); /* the engine is just another subscriber */

    if (err != ESP_OK)
    {
        return err;
    }

    s_inited = true;
    oc_events_register();
    return ESP_OK;
}

/* The sequence itself (wake, reset, negotiate, provision) lives in
 * obd_chip_bringup.c. */
static esp_err_t bringup_run(void)
{
    esp_err_t err = obd_bringup_run(&s_active_baud);

    if (err == ESP_OK)
    {
        s_started = true;
    }

    return err;
}

/** Mark bring-up finished (success OR give-up) and release the gate.
 *  Bit before flag: a gate that read the flag as active must find the
 *  bit set when it reaches the wait. */
static void bringup_finish(void)
{
    xEventGroupSetBits(s_bringup_evt, BRINGUP_DONE_BIT);
    s_bringup_active = false;
}

static void bringup_task(void *arg)
{
    (void)arg;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = bringup_run();

    bringup_finish();

    if (err != ESP_OK)
    {
        /* same degrade-never-halt outcome the boot line used to report:
           the chip stays down, every consumer sees timeouts/errors */
        ESP_LOGE(TAG, "bring-up failed: %s (device degraded)",
                 esp_err_to_name(err));
    }

    /* legacy-parity auto-update (Ali 2026-07-26): a chip not at the
       packaged fw version is flashed to it. Runs AFTER the DONE bit
       (the update path waits on it — ordering matters) and ALSO after a
       FAILED bring-up: a chip stuck in download mode fails bring-up and
       completing the download is its recovery path. A dead wire aborts
       on the VTVERS timeout inside. At the packaged version this costs
       one VTVERS round-trip. */
    if (obd_settings_config()->auto_update)
    {
        esp_err_t up = obd_chip_firmware_update_builtin(false);

        if (up != ESP_OK && up != ESP_ERR_TIMEOUT)
        {
            ESP_LOGW(TAG, "auto fw update did not complete (%s) — will "
                     "re-check next boot", esp_err_to_name(up));
        }
    }

    /* ephemeral task escapes System Monitor — surface the watermark for
       the stack-audit bench (the 2026-07-22 convention) */
    ESP_LOGI(TAG, "bring-up done in %lu ms, stack_hw=%u B",
             (unsigned long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t obd_chip_start(void)
{
    if (!obd_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured (settings boot pass failed); not starting");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started || s_bringup_active)
    {
        return ESP_OK;
    }

    /* async (2026-07-26): the ~2 s ATZ + baud negotiation was half the
       boot — launch it and let boot continue; wire-touching APIs gate
       on the DONE bit. Internal stack: same rule as the RX task (UART
       driver path). Clear-before-arm: a retry after a failed round must
       not leave round 1's DONE bit satisfying round 2's gate. */
    if (s_bringup_evt != NULL)
    {
        xEventGroupClearBits(s_bringup_evt, BRINGUP_DONE_BIT);
    }

    s_bringup_active = true;

    if (xTaskCreate(bringup_task, "obd_bringup", 4096, NULL, 5, NULL)
        != pdPASS)
    {
        /* no task — fall back to the legacy synchronous path */
        esp_err_t err = bringup_run();

        bringup_finish();
        return err;
    }

    return ESP_OK;
}

bool obd_chip_ready(void)
{
    return s_started;
}

esp_err_t obd_chip_stop(void)
{
    /* a stop that races the bring-up task waits it out first (the RX
       task only exists once bring-up finishes) */
    (void)obd_core_bringup_wait(OBD_BRINGUP_WAIT_MS);

    if (!s_started)
    {
        return ESP_OK;
    }

    obd_uart_rx_task_stop();
    s_started = false;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}
