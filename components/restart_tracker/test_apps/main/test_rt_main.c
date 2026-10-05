/**
 * @file test_rt_main.c
 * @brief On-target test app for restart_tracker. Self-driving, one step per
 *        boot, the step number kept in PSRAM `.noinit`:
 *
 *        step 0   a planned restart: the PSRAM state survives a real
 *                 esp_restart() and the next boot consumes the intent (the
 *                 two phases this app always had);
 *        step 1+  the crash note: each step crashes on purpose
 *                 (test_rt_crash_cases.c), the next boot prints the note the
 *                 tracker filed and checks it against what this app knows.
 *
 *        Since 2026-10-05 the same crashes exercise the crash-loop brake and
 *        the stored crash report: no run here lives a second, so each crash
 *        is a quick one (the streak must count them and the verdict turn to
 *        "park" at the third), and each is another crash than the one
 *        before (the report must be stored four times, then no more: the
 *        wear guard's budget), on the real RTC memory and the DUT's NVS.
 *
 *        `tools/testbench/system/rt_target_check.py` (test.ps1 target
 *        restart_tracker) reads the console, compares every note with IDF's
 *        own `Backtrace:` line and gives the verdict. Line formats are
 *        parsed there: keep the `RT ` prefix and the key=value fields.
 *
 *        Test app only: ESP_ERROR_CHECK and panics are the point here.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "restart_tracker.h"

#include "test_rt_cases.h"

#define RT_PROGRESS_MAGIC 0x52545052U /* "RTPR" */
#define RT_FN_SPAN        160U        /* bytes of a crash function a PC may
                                         lie in                              */

/* ---- the run's progress: PSRAM `.noinit`, like the tracker's own state ------ */

typedef struct
{
    uint8_t  mspi_tuning_guard[64]; /* standard §2: MSPI timing tuning writes
                                       the first 64 bytes of this section at
                                       every boot                            */
    uint32_t magic;
    uint32_t step;                  /* the step this boot runs               */
    uint32_t ok_mask;               /* bit k: the report on step k passed    */
    uint32_t unexpected_at_start;   /* the tracker's count when step 0 ran   */
    uint32_t seq[RT_STEP_DONE];     /* the boot that reported on step k      */
    uint32_t budget;                /* report writes left after the boot
                                       before this one                       */
} rt_progress_t;

static EXT_RAM_NOINIT_ATTR rt_progress_t s_prog;

static void progress_commit(void)
{
    ESP_ERROR_CHECK(esp_cache_msync(&s_prog, sizeof(s_prog),
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                    ESP_CACHE_MSYNC_FLAG_UNALIGNED));
}

/* ---- what each step must leave ------------------------------------------------ */

typedef enum
{
    FN_NONE = 0,
    FN_FIRST,   /* the first backtrace entry lies in the step's function  */
    FN_CALLER,  /* a later entry does                                     */
    FN_PC,      /* the note's pc does (a note without its backtrace)      */
} rt_fn_check_t;

typedef struct
{
    const char *name;
    bool        note;        /* the step ends in a crash that leaves a note */
    uint8_t     kind;
    const char *reason;      /* exact; NULL: not checked                    */
    const char *text_has;    /* part of the abort text; NULL: not checked   */
    const char *task;        /* exact; NULL: not checked                    */
    int8_t      in_isr;      /* -1: not checked                             */
    int8_t      complete;
    int8_t      corrupt;
    int8_t      core;
    bool        bad_addr;    /* excvaddr must be RT_BAD_ADDR                */
    uint8_t     fn;          /* rt_fn_check_t                               */
    uint32_t    reset;       /* esp_reset_reason_t of the boot after        */
} rt_expect_t;

#define X RESTART_TRACKER_CRASH_EXCEPTION
#define A RESTART_TRACKER_CRASH_ABORT
#define W RESTART_TRACKER_CRASH_INT_WDT

static const rt_expect_t EXPECT[RT_STEP_DONE] =
{
    [RT_STEP_PLANNED] =
        { "planned_restart", false, 0, NULL, NULL, NULL,
          -1, -1, -1, -1, false, FN_NONE, ESP_RST_SW },
    [RT_STEP_STORE] =
        { "store_in_task", true, X, "StoreProhibited", NULL, "rt_int",
          0, 1, 0, 0, true, FN_FIRST, ESP_RST_PANIC },
    [RT_STEP_STORE_PSRAM] =
        { "store_on_psram_stack", true, X, "StoreProhibited", NULL,
          "rt_psram", 0, 1, 0, 0, true, FN_FIRST, ESP_RST_PANIC },
    [RT_STEP_ABORT] =
        { "abort", true, A, "", "abort() was called at PC 0x", "main",
          0, 1, 0, 0, false, FN_NONE, ESP_RST_PANIC },
    [RT_STEP_ASSERT] =
        { "assert", true, A, "", "assert failed: rt_crash_assert", "main",
          0, 1, 0, 0, false, FN_CALLER, ESP_RST_PANIC },
    [RT_STEP_STACK_OVERFLOW] =
        { "stack_overflow", true, A, "", "stack overflow in task rt_ovf",
          NULL, -1, 1, -1, -1, false, FN_NONE, ESP_RST_PANIC },
    [RT_STEP_WDT_CPU0] =
        { "int_wdt_cpu0", true, W, "Interrupt wdt timeout on CPU0", NULL,
          "rt_spin", -1, 1, 0, 0, false, FN_FIRST, ESP_RST_INT_WDT },
    [RT_STEP_WDT_CPU1] =
        { "int_wdt_cpu1", true, W, "Interrupt wdt timeout on CPU1", NULL,
          "rt_spin", -1, 1, 0, 1, false, FN_FIRST, ESP_RST_INT_WDT },
    [RT_STEP_ISR] =
        { "store_in_isr", true, X, "StoreProhibited", NULL, NULL,
          1, 1, 0, -1, true, FN_FIRST, ESP_RST_PANIC },
    [RT_STEP_CALL_NULL] =
        { "call_null", true, X, "InstrFetchProhibited", NULL, "main",
          0, 1, 0, 0, false, FN_CALLER, ESP_RST_PANIC },
    [RT_STEP_CACHE_OFF] =
        { "store_cache_off", true, X, "StoreProhibited", NULL, "main",
          -1, 1, 0, 0, true, FN_FIRST, ESP_RST_PANIC },
    [RT_STEP_CORRUPT_CHAIN] =
        { "corrupt_chain", true, X, "StoreProhibited", NULL, "main",
          0, 1, 1, 0, true, FN_FIRST, ESP_RST_PANIC },
    [RT_STEP_STAGE_B_FAULT] =
        { "stage_b_fault", true, X, NULL, NULL, NULL,
          -1, 0, -1, 0, true, FN_PC, ESP_RST_PANIC },
    [RT_STEP_STAGE_B_HANG] =
        { "stage_b_hang", false, 0, NULL, NULL, NULL,
          -1, -1, -1, -1, false, FN_NONE, ESP_RST_WDT },
    [RT_STEP_GARBAGE] =
        { "garbage_store", false, 0, NULL, NULL, NULL,
          -1, -1, -1, -1, false, FN_NONE, ESP_RST_SW },
};

#undef X
#undef A
#undef W

/* ---- the report on the step before ---------------------------------------------- */

static bool s_step_ok;

static void check(uint32_t step, const char *what, bool ok)
{
    printf("RT CHECK step=%" PRIu32 " %s %s\n", step, what,
           ok ? "ok" : "FAIL");

    if (!ok)
    {
        s_step_ok = false;
    }
}

static bool in_fn(uint32_t pc, const void *fn)
{
    uint32_t lo = (uint32_t)(uintptr_t)fn;

    return fn != NULL && pc >= lo && pc < lo + RT_FN_SPAN;
}

static void print_note(uint32_t step, bool found,
                       const restart_tracker_crash_t *c,
                       const restart_tracker_record_t *rec)
{
    printf("RT NOTE step=%" PRIu32 " found=%d reset=%s planned=%d", step,
           found, restart_tracker_reset_reason_to_str(rec->actual_reset_reason),
           rec->was_planned);

    if (found)
    {
        printf(" kind=%s cause=%" PRIu32 " pc=0x%08" PRIx32
               " excvaddr=0x%08" PRIx32 " core=%u complete=%d pseudo=%d"
               " in_isr=%d nested=%d early=%d corrupt=%d more=%d"
               " uptime=%" PRIu32 " elf=%s task=\"%s\" reason=\"%s\""
               " text=\"%s\"",
               restart_tracker_crash_kind_to_str(c->kind), c->cause, c->pc,
               c->excvaddr, (unsigned)c->core, c->complete, c->pseudo,
               c->in_isr, c->nested, c->early, c->bt_corrupt, c->bt_more,
               c->uptime_s, c->elf_sha, c->task, c->reason, c->text);
    }

    printf("\n");

    if (found && c->bt_len > 0)
    {
        printf("RT NOTEBT step=%" PRIu32, step);

        for (int i = 0; i < c->bt_len; i++)
        {
            printf(" 0x%08" PRIx32, c->bt[i]);
        }

        printf("\n");
    }

    if (found && c->bt2_len > 0)
    {
        printf("RT NOTEBT2 step=%" PRIu32 " core=%u", step,
               (unsigned)c->bt2_core);

        for (int i = 0; i < c->bt2_len; i++)
        {
            printf(" 0x%08" PRIx32, c->bt2[i]);
        }

        printf("\n");
    }
}

static void check_note(uint32_t step, const rt_expect_t *e,
                       const restart_tracker_crash_t *c)
{
    const void *fn = rt_case_fn((rt_step_t)step);

    check(step, "kind", c->kind == e->kind);

    if (e->complete >= 0)
    {
        check(step, "complete", c->complete == (e->complete != 0));
    }

    if (e->core >= 0)
    {
        check(step, "core", c->core == (uint8_t)e->core);
    }

    if (e->bad_addr)
    {
        check(step, "excvaddr", c->excvaddr == RT_BAD_ADDR);
    }

    if (e->fn == FN_PC)
    {
        check(step, "pc_in_function", in_fn(c->pc, fn));
    }

    if (!c->complete)
    {
        return; /* the head is all there is */
    }

    if (e->reason != NULL)
    {
        check(step, "reason", strcmp(c->reason, e->reason) == 0);
    }

    if (e->text_has != NULL)
    {
        check(step, "text", strstr(c->text, e->text_has) != NULL);
    }

    if (e->task != NULL)
    {
        check(step, "task", strcmp(c->task, e->task) == 0);
    }

    if (e->in_isr >= 0)
    {
        check(step, "in_isr", c->in_isr == (e->in_isr != 0));
    }

    if (e->corrupt >= 0)
    {
        check(step, "bt_corrupt", c->bt_corrupt == (e->corrupt != 0));
    }

    check(step, "image_id", strlen(c->elf_sha) == 16);
    check(step, "not_early", !c->early);
    check(step, "backtrace", c->bt_len >= 2);

    if (e->fn == FN_FIRST)
    {
        check(step, "bt0_in_function", in_fn(c->bt[0], fn));
    }
    else if (e->fn == FN_CALLER)
    {
        bool hit = false;

        for (int i = 1; i < c->bt_len; i++)
        {
            hit = hit || in_fn(c->bt[i], fn);
        }

        check(step, "caller_in_backtrace", hit);
    }
}

/** The stored report is the crash of this note. Aborts share their pc:
 *  the text and the callers tell them apart. */
static bool same_crash(const restart_tracker_crash_t *a,
                       const restart_tracker_crash_t *b)
{
    return a->kind == b->kind && a->pc == b->pc &&
           strcmp(a->text, b->text) == 0 && strcmp(a->task, b->task) == 0 &&
           a->bt_len == b->bt_len &&
           memcmp(a->bt, b->bt, a->bt_len * sizeof(a->bt[0])) == 0;
}

/** The crash-loop brake and the stored report after step `step`. Steps 1
 *  to 13 are `step` quick crashes in a row, each another crash than the one
 *  before; step 0 began with the run marked settled (a full budget). */
static void report_brake(uint32_t step, bool note_found,
                         const restart_tracker_crash_t *note)
{
    static restart_tracker_report_t rep;
    restart_tracker_brake_t brake = { 0 };
    bool have = restart_tracker_get_report(&rep) == ESP_OK;
    bool park = step >= RESTART_TRACKER_BRAKE_STREAK;

    (void)restart_tracker_get_brake(&brake);
    printf("RT BRAKE step=%" PRIu32 " verdict=%s streak=%u parks=%u budget=%u"
           " budget_before=%" PRIu32 "\n", step,
           restart_tracker_boot_mode_to_str(brake.verdict),
           (unsigned)brake.streak, (unsigned)brake.parks,
           (unsigned)brake.report_budget, s_prog.budget);
    printf("RT REPORT step=%" PRIu32 " found=%d pc=0x%08" PRIx32
           " streak=%u parked=%d\n", step, have, have ? rep.crash.pc : 0U,
           have ? (unsigned)rep.streak : 0U, have && rep.parked);

    if (step == RT_STEP_PLANNED || step == RT_STEP_GARBAGE)
    {
        /* a planned restart ends a streak; the garbage step also wiped the
           store the count lives in: both start over, with a full budget */
        check(step, "brake_at_rest",
              brake.verdict == RESTART_TRACKER_BOOT_NORMAL &&
              brake.streak == 0U && brake.report_budget == 4U);
        return;
    }

    check(step, "brake_streak", brake.streak == step);
    check(step, "brake_verdict",
          brake.verdict == (park ? RESTART_TRACKER_BOOT_PARK
                                 : RESTART_TRACKER_BOOT_NORMAL));

    if (!note_found)
    {
        return; /* the hang step may leave nothing to store */
    }

    if (s_prog.budget > 0U)
    {
        check(step, "report_stored",
              have && same_crash(&rep.crash, note) &&
              brake.report_budget == s_prog.budget - 1U);
        check(step, "report_says_parked", have && rep.parked == park);
    }
    else
    {
        check(step, "report_budget_spent",
              brake.report_budget == 0U &&
              (!have || !same_crash(&rep.crash, note)));
    }
}

static void report(uint32_t step, const restart_tracker_state_t *st,
                   const restart_tracker_record_t *rec)
{
    static restart_tracker_crash_t crash;
    const rt_expect_t *e = &EXPECT[step];
    bool found = restart_tracker_get_crash(rec->sequence, &crash) == ESP_OK;

    s_step_ok = true;
    print_note(step, found, &crash, rec);
    check(step, "reset_reason", rec->actual_reset_reason == e->reset);

    if (step == RT_STEP_PLANNED)
    {
        /* the app's two phases of old: the intent of step 0 was consumed */
        printf("PHASE2 planned=%d reason=%s source=%s flags=0x%lX\n",
               rec->was_planned,
               restart_tracker_planned_reason_to_str(
                   (restart_tracker_planned_reason_t)rec->planned_reason),
               restart_tracker_source_to_str(
                   (restart_tracker_source_t)rec->source),
               (unsigned long)rec->flags);
        printf("SURVIVED boots=%lu unexpected=%lu history_kept=%d\n",
               (unsigned long)st->boot_count,
               (unsigned long)(st->unexpected_reset_count -
                               s_prog.unexpected_at_start),
               st->boot_count >= 2);
        printf("CLASSIFY reset=%s unexpected_count=%lu\n",
               restart_tracker_reset_reason_to_str(rec->actual_reset_reason),
               (unsigned long)(st->unexpected_reset_count -
                               s_prog.unexpected_at_start));
        check(step, "planned_intent",
              rec->was_planned &&
              rec->planned_reason ==
                  RESTART_TRACKER_PLANNED_REASON_USER_REQUEST &&
              rec->source == RESTART_TRACKER_SOURCE_CONSOLE &&
              rec->flags == 0xC0FFEE);
        check(step, "history_kept", st->boot_count >= 2);
    }

    if (step == RT_STEP_STAGE_B_HANG)
    {
        /* measured, not demanded: does RTC memory keep the head over the RTC
           watchdog's reset? (TASK_crash_note.md, section 15 item 2) */
        printf("RT MEASURE hang_note_found=%d hang_note_complete=%d\n",
               found, found && crash.complete);
    }
    else if (!e->note)
    {
        check(step, "no_note", !found);
    }
    else
    {
        check(step, "note_found", found);

        if (found)
        {
            check_note(step, e, &crash);
        }
    }

    /* every crash step is one unexpected reset more, a planned one is none */
    uint32_t want = (step > RT_STEP_STAGE_B_HANG) ? RT_STEP_STAGE_B_HANG
                                                  : step;

    check(step, "unexpected_count",
          st->unexpected_reset_count - s_prog.unexpected_at_start == want);

    if (step == RT_STEP_STORE)
    {
        uint32_t us = 0;

        if (rt_case_probe_take(&us))
        {
            printf("RT MEASURE stage_b_us=%" PRIu32 "\n", us);
        }

        printf("RT MEASURE rtc_noinit_bytes=%" PRIu32 "\n",
               rt_case_rtc_noinit_bytes());
    }

    if (step == RT_STEP_CORRUPT_CHAIN)
    {
        /* notes stay: those of one and two boots ago are still filed */
        restart_tracker_crash_t old;

        check(step, "note_kept_1_boot",
              restart_tracker_get_crash(s_prog.seq[RT_STEP_CACHE_OFF],
                                        &old) == ESP_OK &&
              old.excvaddr == RT_BAD_ADDR);
        check(step, "note_kept_2_boots",
              restart_tracker_get_crash(s_prog.seq[RT_STEP_CALL_NULL],
                                        &old) == ESP_OK &&
              old.cause == 20U /* InstrFetchProhibited */);
    }

    if (step == RT_STEP_STAGE_B_HANG)
    {
        restart_tracker_crash_t old;

        /* measured too: the notes filed before, over the same reset */
        printf("RT MEASURE notes_kept_over_rtc_wdt_reset=%d\n",
               restart_tracker_get_crash(s_prog.seq[RT_STEP_STAGE_B_FAULT],
                                         &old) == ESP_OK);
    }

    if (step == RT_STEP_GARBAGE)
    {
        restart_tracker_crash_t old;

        check(step, "old_notes_gone",
              restart_tracker_get_crash(s_prog.seq[RT_STEP_STAGE_B_FAULT],
                                        &old) == ESP_ERR_NOT_FOUND &&
              restart_tracker_get_crash(s_prog.seq[RT_STEP_CORRUPT_CHAIN],
                                        &old) == ESP_ERR_NOT_FOUND);
    }

    report_brake(step, found, &crash);

    printf("RT RESULT step=%" PRIu32 " %s %s\n", step, e->name,
           s_step_ok ? "ok" : "FAIL");

    if (s_step_ok)
    {
        s_prog.ok_mask |= (1U << step);
    }

    s_prog.seq[step] = rec->sequence;
}

/* ---- one boot ---------------------------------------------------------------------- */

void app_main(void)
{
    /* the DUT's own NVS partition, as the firmware opens it: the tracker
       stores its crash report there. Never erased from here (it holds the
       firmware's WiFi calibration, bonds and fault codes). */
    esp_err_t nvs = nvs_flash_init();

    printf("RT NVS init=%s\n", esp_err_to_name(nvs));

    ESP_ERROR_CHECK(restart_tracker_init());
    ESP_ERROR_CHECK(restart_tracker_start());

    static restart_tracker_state_t st;
    restart_tracker_record_t rec;

    ESP_ERROR_CHECK(restart_tracker_get_state(&st));
    ESP_ERROR_CHECK(restart_tracker_get_latest_record(&rec));

    printf("BOOT count=%lu seq=%lu reason=%s planned=%d\n",
           (unsigned long)st.boot_count, (unsigned long)rec.sequence,
           restart_tracker_reset_reason_to_str(rec.actual_reset_reason),
           rec.was_planned);

    /* a power-on or the flasher's reset starts the run; anything else is
       the run's own next boot */
    if (rec.actual_reset_reason == ESP_RST_POWERON ||
        rec.actual_reset_reason == ESP_RST_EXT ||
        s_prog.magic != RT_PROGRESS_MAGIC || s_prog.step > RT_STEP_DONE)
    {
        memset(&s_prog, 0, sizeof(s_prog));
        s_prog.magic = RT_PROGRESS_MAGIC;
        s_prog.unexpected_at_start = st.unexpected_reset_count;
        /* a known start for the brake, whatever the firmware left in RTC
           memory: no streak, the report budget full */
        (void)restart_tracker_settle(true);
    }

    uint32_t step = s_prog.step;

    printf("RT STEP %" PRIu32 " %s\n", step,
           (step < RT_STEP_DONE) ? EXPECT[step].name : "done");

    if (step > 0)
    {
        report(step - 1U, &st, &rec);
    }

    if (step >= RT_STEP_DONE)
    {
        uint32_t all = (1U << RT_STEP_DONE) - 1U;
        int ok = __builtin_popcount(s_prog.ok_mask & all);

        /* as found: the firmware that is flashed back must not show this
           run's crashes as the device's stored report */
        static restart_tracker_report_t gone;
        esp_err_t cleared = restart_tracker_clear_report();

        printf("RT CLEANUP report_cleared=%d\n",
               cleared == ESP_OK &&
               restart_tracker_get_report(&gone) == ESP_ERR_NOT_FOUND);
        printf("RT SUMMARY ok=%d of=%d\n", ok, (int)RT_STEP_DONE);
        s_prog.step = 0; /* a reset now starts over */
        progress_commit();
        printf("TEST DONE\n");
        return;
    }

    restart_tracker_brake_t brake = { 0 };

    (void)restart_tracker_get_brake(&brake);
    s_prog.budget = brake.report_budget; /* what the next boot starts from */
    s_prog.step = step + 1U;
    progress_commit();
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the lines out */

    if (step == RT_STEP_PLANNED)
    {
        printf("PHASE1 marking planned restart and rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_USER_REQUEST,
                                RESTART_TRACKER_SOURCE_CONSOLE, 0xC0FFEE);
    }

    if (step == RT_STEP_GARBAGE)
    {
        rt_case_garbage_fill();
        restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_USER_REQUEST,
                                RESTART_TRACKER_SOURCE_CONSOLE, 0);
    }

    rt_case_run((rt_step_t)step);

    /* a crash step that came back did not crash */
    printf("RT RESULT step=%" PRIu32 " %s FAIL (no crash)\n", step,
           EXPECT[step].name);
    esp_restart();
}
