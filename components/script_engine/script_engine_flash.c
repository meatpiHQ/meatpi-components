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

/**
 * @file script_engine_flash.c
 * @brief The state of a flash run, read by the web API while the runner
 *        writes it: phase, progress, the ECU's state, the operator prompt,
 *        a log of numbered lines, and the device lock with its NVS record
 *        (rules in script_engine_flash_core.c; bindings in
 *        script_engine_flash_bind.c).
 *
 * The NVS record ("se_flash"/"commit") is written by flash_commit() before
 * the script goes on, and erased only when a committed run ends well or an
 * operator releases a failed one. A restart in between comes back locked
 * (recovery), so nothing else on the WiCAN can transmit to a half-written
 * ECU until someone decides.
 */
#include "script_engine_flash.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "flash_jobs.h"
#include "obd_chip.h"
#include "obd_gate.h"
#include "ota_manager.h"

#include "script_engine.h"
#include "script_engine_private.h"

static const char *TAG = "script_engine";

#define SE_LOG_LINES   96
#define SE_LOG_LEN     120
#define SE_LABEL_MAX   64  /* commit label: stored, read back, shown      */
#define SE_NVS_NS      "se_flash"
#define SE_NVS_KEY     "commit"

typedef struct
{
    uint32_t n;
    uint32_t ms;
    char     msg[SE_LOG_LEN];
} se_log_line_t;

static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;          /* internal: FreeRTOS object */
static SemaphoreHandle_t s_answer_sem;
static StaticSemaphore_t s_answer_buf;      /* internal: FreeRTOS object */

static se_lock_state_t s_lock;
static bool            s_lock_effects;     /* bus lock + diag hold applied */
static bool            s_run_active;
static bool            s_async;            /* the run in flight is async   */
static bool            s_kill_pending;     /* Stop before the commit       */
static uint32_t        s_run_id;           /* the last async run           */
static uint32_t        s_next_run_id;
static void           *s_runner;
static const int       s_lock_token;       /* obd_gate diag-hold owner     */

/* the last async run's state, guarded by s_mx (runner writes, httpd reads);
   a sync, rule or console run changes none of it */
EXT_RAM_BSS_ATTR static char s_phase[48];
EXT_RAM_BSS_ATTR static char s_ecu_state[16];
EXT_RAM_BSS_ATTR static char s_commit_label[SE_LABEL_MAX];
EXT_RAM_BSS_ATTR static char s_prompt[200];
EXT_RAM_BSS_ATTR static char s_choices[120];
EXT_RAM_BSS_ATTR static char s_answer[96];
EXT_RAM_BSS_ATTR static char s_error[200];
static uint32_t s_done, s_total;
static bool     s_prompt_open;
static bool     s_abort;
static bool     s_have_result;
static bool     s_result_ok;
static cJSON   *s_params;

EXT_RAM_BSS_ATTR static se_log_line_t s_log[SE_LOG_LINES];
static uint32_t s_log_next = 1;

static void lock_mx(void)
{
    if (s_mx == NULL)
    {
        s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
        s_answer_sem = xSemaphoreCreateBinaryStatic(&s_answer_buf);
    }

    xSemaphoreTake(s_mx, portMAX_DELAY);
}

static void unlock_mx(void)
{
    xSemaphoreGive(s_mx);
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", (src != NULL) ? src : "");
}

/* While the lock holds, the OBD chip sends nothing (obd_gate's bus lock)
 * and AutoPID's pollers stay off (a diag hold of the lock's own, apart
 * from the run's exclusive hold). Applied on every change of s_lock. */
static void apply_lock_effects(void)
{
    bool held = se_lock_held(s_lock);

    if (held == s_lock_effects)
    {
        return;
    }

    s_lock_effects = held;
    obd_gate_set_bus_lock(held);
    obd_gate_diag_hold(&s_lock_token, held);
}

/* ---- the NVS record (internal stack: flash_jobs) -------------------------- */

typedef struct
{
    const char *label; /* write: the label; NULL: erase                     */
    bool        found; /* read: a record is (or may be) there               */
    char        read[SE_LABEL_MAX];
} se_nvs_job_t;

static esp_err_t nvs_write_job(void *arg)
{
    se_nvs_job_t *j = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SE_NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }

    if (j->label != NULL)
    {
        err = nvs_set_str(h, SE_NVS_KEY, j->label);
    }
    else
    {
        err = nvs_erase_key(h, SE_NVS_KEY);
        err = (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* Fails closed: only "no namespace" or "no key" mean no record. Anything
   else (a label that does not fit, NVS not initialised, corruption) is
   taken as a record, so a restart mid-flash can never come back unlocked. */
static esp_err_t nvs_read_job(void *arg)
{
    se_nvs_job_t *j = arg;
    nvs_handle_t h;
    size_t len = sizeof(j->read);
    esp_err_t err = nvs_open(SE_NVS_NS, NVS_READONLY, &h);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK; /* the namespace was never written */
    }

    if (err == ESP_OK)
    {
        err = nvs_get_str(h, SE_NVS_KEY, j->read, &len);
        nvs_close(h);

        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            return ESP_OK;
        }
    }

    j->found = true;

    if (err != ESP_OK)
    {
        copy(j->read, sizeof(j->read), "unreadable");
    }

    return err;
}

void se_flash_boot(void)
{
    se_nvs_job_t j = { 0 };
    esp_err_t err = flash_jobs_run(nvs_read_job, &j);

    if (err != ESP_OK && !j.found)
    {
        j.found = true; /* the read itself failed: fail closed */
        copy(j.read, sizeof(j.read), "unreadable");
    }

    lock_mx();
    s_lock = se_lock_on_boot(j.found);

    if (j.found)
    {
        copy(s_commit_label, sizeof(s_commit_label), j.read);
        copy(s_ecu_state, sizeof(s_ecu_state), "unknown");
    }

    unlock_mx();
    apply_lock_effects();

    if (j.found)
    {
        ESP_LOGW(TAG, "restarted during an ECU flash (\"%s\"%s%s): locked "
                 "until a recovery run finishes or the lock is released",
                 j.read, (err != ESP_OK) ? ", " : "",
                 (err != ESP_OK) ? esp_err_to_name(err) : "");
    }
}

/* ---- the run's lifecycle -------------------------------------------------- */

void se_flash_set_runner(void *task_handle)
{
    s_runner = task_handle;
}

uint32_t se_flash_next_run_id(void)
{
    lock_mx();

    uint32_t id = ++s_next_run_id;

    unlock_mx();
    return id;
}

void se_flash_run_begin(cJSON *params, bool async, uint32_t run_id)
{
    lock_mx();
    s_run_active = true;
    s_async = async;
    s_abort = false;
    s_kill_pending = false;
    s_prompt_open = false;

    if (async)
    {
        cJSON_Delete(s_params);
        s_params = params;
        params = NULL;
        s_run_id = run_id;
        s_phase[0] = '\0';
        s_done = 0;
        s_total = 0;
        s_have_result = false;
        s_error[0] = '\0';

        if (s_lock == SE_LOCK_IDLE)
        {
            copy(s_ecu_state, sizeof(s_ecu_state), "untouched");
        }
    }

    unlock_mx();
    cJSON_Delete(params); /* a sync run has no params to keep */
    xSemaphoreTake(s_answer_sem, 0); /* no stale answer */
}

void se_flash_run_end(bool ok, const char *error)
{
    se_tp_epilogue();
    lock_mx();

    se_lock_state_t was = s_lock;
    bool aborted = (was == SE_LOCK_COMMITTED) && s_abort;

    s_lock = se_lock_on_run_end(s_lock, ok, s_abort);
    s_run_active = false;
    s_prompt_open = false;

    if (s_async)
    {
        s_have_result = true;
        s_result_ok = ok && !aborted;
        copy(s_error, sizeof(s_error),
             !ok ? error : aborted ? "aborted after flash_commit()" : "");
    }

    unlock_mx();
    apply_lock_effects();

    if (was == SE_LOCK_COMMITTED && s_lock == SE_LOCK_IDLE)
    {
        se_nvs_job_t j = { .label = NULL };

        if (flash_jobs_run(nvs_write_job, &j) != ESP_OK)
        {
            ESP_LOGW(TAG, "flash record not cleared: the next boot locks");
        }

        ESP_LOGI(TAG, "ECU flash run finished");
    }
    else if (was == SE_LOCK_COMMITTED)
    {
        ESP_LOGW(TAG, "ECU flash run %s after its commit: locked",
                 aborted && ok ? "aborted" : "failed");
    }
}

se_lock_state_t se_flash_lock_state(void)
{
    return s_lock;
}

bool se_flash_lock_held(void)
{
    return se_lock_held(s_lock);
}

bool se_flash_run_is_async(void)
{
    return s_run_active && s_async;
}

bool se_flash_budget_exempt(void)
{
    return s_run_active && s_lock == SE_LOCK_COMMITTED;
}

bool se_flash_stop(void)
{
    lock_mx();

    bool request = s_run_active && s_lock == SE_LOCK_COMMITTED;

    if (request)
    {
        s_abort = true;
    }
    else if (s_run_active)
    {
        s_kill_pending = true; /* a commit under way refuses to complete */
    }

    unlock_mx();
    return request;
}

esp_err_t se_flash_release(void)
{
    lock_mx();

    se_lock_state_t was = s_lock;

    if (s_run_active || !(was == SE_LOCK_FAILED || was == SE_LOCK_RECOVERY))
    {
        unlock_mx();
        return ESP_ERR_INVALID_STATE;
    }

    /* the record first, under the mutex (the flash_jobs worker never takes
       it): no run can start between the erase and the state change, and a
       failed erase leaves the lock as it was */
    se_nvs_job_t j = { .label = NULL };
    esp_err_t err = flash_jobs_run(nvs_write_job, &j);

    if (err == ESP_OK)
    {
        s_lock = se_lock_on_release(was);
    }

    unlock_mx();
    apply_lock_effects();

    if (err == ESP_OK)
    {
        ESP_LOGW(TAG, "flash lock released by the operator (was %s)",
                 se_lock_state_str(was));
    }
    else
    {
        ESP_LOGW(TAG, "flash lock kept: the record was not cleared (%s)",
                 esp_err_to_name(err));
    }

    return err;
}

/* ---- what the bindings change (called on the runner) ---------------------- */

esp_err_t se_flash_commit(const char *label)
{
    char stored[SE_LABEL_MAX]; /* what NVS keeps is what the boot reads */

    copy(stored, sizeof(stored), (label != NULL && label[0]) ? label : "flash");

    if (!se_flash_run_is_async())
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (ota_manager_busy() || obd_chip_update_in_progress() ||
        obd_chip_kept_for_update())
    {
        return ESP_ERR_INVALID_STATE; /* it would restart or reset the chip */
    }

    se_nvs_job_t j = { .label = stored };
    esp_err_t err = flash_jobs_run(nvs_write_job, &j);

    if (err != ESP_OK)
    {
        return err; /* not recorded: the script must not go on */
    }

    lock_mx();

    if (s_kill_pending)
    {
        /* Stop came while the record was written: never commit */
        unlock_mx();
        j.label = NULL;
        (void)flash_jobs_run(nvs_write_job, &j);
        return ESP_ERR_NOT_ALLOWED;
    }

    s_lock = se_lock_on_commit(s_lock);
    copy(s_commit_label, sizeof(s_commit_label), stored);
    unlock_mx();
    apply_lock_effects();
    ESP_LOGI(TAG, "ECU flash committed (\"%s\"): the device is locked", stored);
    return ESP_OK;
}

void se_flash_set_phase(const char *phase)
{
    lock_mx();

    if (s_async)
    {
        copy(s_phase, sizeof(s_phase), phase);
    }

    unlock_mx();
}

void se_flash_set_progress(uint32_t done, uint32_t total)
{
    lock_mx();

    if (s_async)
    {
        s_done = done;
        s_total = total;
    }

    unlock_mx();
}

void se_flash_set_ecu_state(const char *state)
{
    lock_mx();
    copy(s_ecu_state, sizeof(s_ecu_state), state);
    unlock_mx();
}

bool se_flash_abort_requested(void)
{
    return s_abort;
}

void se_flash_log(const char *msg)
{
    lock_mx();

    se_log_line_t *l = &s_log[s_log_next % SE_LOG_LINES];

    l->n = s_log_next++;
    l->ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    copy(l->msg, sizeof(l->msg), msg);
    unlock_mx();
    ESP_LOGD(TAG, "flash: %s", msg);
}

const cJSON *se_flash_params(void)
{
    return s_params; /* written only between runs */
}

/* Open a prompt and wait for its one answer (polling @p alive every 200 ms:
   false ends the wait with ESP_ERR_INVALID_STATE). */
esp_err_t se_flash_ask(const char *text, const char *choices,
                       bool (*alive)(void *), void *ctx,
                       char *answer, size_t cap)
{
    lock_mx();
    xSemaphoreTake(s_answer_sem, 0); /* nothing left from an earlier prompt */
    copy(s_prompt, sizeof(s_prompt), text);
    copy(s_choices, sizeof(s_choices), choices);
    s_prompt_open = true;
    unlock_mx();

    while (xSemaphoreTake(s_answer_sem, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        if (!alive(ctx))
        {
            lock_mx();
            s_prompt_open = false;
            unlock_mx();
            return ESP_ERR_INVALID_STATE;
        }
    }

    lock_mx();
    copy(answer, cap, s_answer);
    unlock_mx();
    return ESP_OK;
}

esp_err_t se_flash_answer(const char *value)
{
    lock_mx();

    bool open = s_prompt_open;

    if (open)
    {
        /* one answer per prompt: a second one finds it closed (409) */
        copy(s_answer, sizeof(s_answer), value);
        s_prompt_open = false;
        xSemaphoreGive(s_answer_sem);
    }

    unlock_mx();
    return open ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ---- the gates (wired at init) ---------------------------------------------- */

bool script_engine_tx_allowed(void)
{
    return se_lock_tx_allowed(s_lock,
                              s_runner != NULL &&
                              xTaskGetCurrentTaskHandle() == s_runner);
}

bool script_engine_http_allowed(bool read_only, const char *uri)
{
    return se_lock_http_allowed(s_lock, read_only, uri, s_run_active);
}

const char *script_engine_busy_reason(void)
{
    /* a committed run is writing an ECU: no sleep entry and no planned
       restart under it (a failed or recovering lock persists in NVS, so
       sleeping or restarting then loses nothing) */
    return (s_run_active && s_lock == SE_LOCK_COMMITTED)
               ? "an ECU is being flashed"
               : NULL;
}

/* ---- the status document ---------------------------------------------------- */

cJSON *se_flash_status(uint32_t since)
{
    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    lock_mx();
    cJSON_AddStringToObject(o, "lock", se_lock_state_str(s_lock));
    cJSON_AddBoolToObject(o, "running", s_run_active);
    cJSON_AddBoolToObject(o, "async", s_run_active && s_async);
    cJSON_AddNumberToObject(o, "run", s_run_id);
    cJSON_AddStringToObject(o, "phase", s_phase);
    cJSON_AddStringToObject(o, "ecu_state", s_ecu_state);
    cJSON_AddStringToObject(o, "commit", s_commit_label);
    cJSON_AddBoolToObject(o, "abort_requested", s_abort);

    cJSON *p = cJSON_AddObjectToObject(o, "progress");

    cJSON_AddNumberToObject(p, "done", s_done);
    cJSON_AddNumberToObject(p, "total", s_total);

    if (s_prompt_open)
    {
        cJSON *q = cJSON_AddObjectToObject(o, "prompt");

        cJSON_AddStringToObject(q, "text", s_prompt);
        cJSON_AddStringToObject(q, "choices", s_choices);
    }

    if (s_have_result)
    {
        cJSON *r = cJSON_AddObjectToObject(o, "result");

        cJSON_AddNumberToObject(r, "run", s_run_id);
        cJSON_AddBoolToObject(r, "ok", s_result_ok);
        cJSON_AddStringToObject(r, "error", s_error);
    }

    /* a cursor past the end means the device restarted since: start over */
    if (since >= s_log_next)
    {
        since = 0;
    }

    cJSON *log = cJSON_AddArrayToObject(o, "log");
    uint32_t first = (s_log_next > SE_LOG_LINES) ? s_log_next - SE_LOG_LINES : 1;

    if (since + 1 > first)
    {
        first = since + 1;
    }

    for (uint32_t n = first; n < s_log_next; n++)
    {
        const se_log_line_t *l = &s_log[n % SE_LOG_LINES];
        cJSON *e = cJSON_CreateObject();

        cJSON_AddNumberToObject(e, "n", l->n);
        cJSON_AddNumberToObject(e, "ms", l->ms);
        cJSON_AddStringToObject(e, "msg", l->msg);
        cJSON_AddItemToArray(log, e);
    }

    cJSON_AddNumberToObject(o, "next", s_log_next);
    unlock_mx();
    return o;
}
