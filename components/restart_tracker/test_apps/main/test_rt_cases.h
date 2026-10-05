/**
 * @file test_rt_cases.h
 * @brief The steps of the restart_tracker on-target run (one per boot) and
 *        the deliberate crashes behind them (test_rt_crash_cases.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    RT_STEP_PLANNED = 0,     /* a planned restart: no note                   */
    RT_STEP_STORE,           /* invalid store in a task, internal stack      */
    RT_STEP_STORE_PSRAM,     /* the same, the task's stack in PSRAM          */
    RT_STEP_ABORT,           /* abort()                                      */
    RT_STEP_ASSERT,          /* a failing assert                             */
    RT_STEP_STACK_OVERFLOW,  /* the stack overflow hook                      */
    RT_STEP_WDT_CPU0,        /* interrupts off and spin, core 0              */
    RT_STEP_WDT_CPU1,        /* the same, core 1                             */
    RT_STEP_ISR,             /* invalid store inside a timer interrupt       */
    RT_STEP_CALL_NULL,       /* call through a NULL function pointer         */
    RT_STEP_CACHE_OFF,       /* invalid store with the flash cache disabled  */
    RT_STEP_CORRUPT_CHAIN,   /* a spilled stack pointer overwritten, then a
                                fault: the walk must stop, not crash         */
    RT_STEP_STAGE_B_FAULT,   /* the note's own stage B made to fault         */
    RT_STEP_STAGE_B_HANG,    /* stage B made to hang: the RTC watchdog       */
    RT_STEP_GARBAGE,         /* the note store filled with 0xA5              */
    RT_STEP_DONE,            /* reports on the last step, then TEST DONE     */
} rt_step_t;

#define RT_BAD_ADDR   0x0000BAD0U /* every deliberate store faults here      */
#define RT_BAD_ADDR_B 0x0000BAD4U /* the store injected into stage B         */

/** Run a step's crash. Returns only for a step this file does not own. */
void rt_case_run(rt_step_t step);

/** A function one of the note's program counters must lie in, or NULL. */
const void *rt_case_fn(rt_step_t step);

/** Microseconds from stage B's second statement to the reset call of the
 *  last crash that was measured; false when there is no measurement. */
bool rt_case_probe_take(uint32_t *stage_b_us);

/** Size of `.rtc_noinit`: the note store plus this app's probe. */
uint32_t rt_case_rtc_noinit_bytes(void);

/** Fill all of `.rtc_noinit` with 0xA5 (what a power-on may leave). */
void rt_case_garbage_fill(void);

#ifdef __cplusplus
}
#endif
