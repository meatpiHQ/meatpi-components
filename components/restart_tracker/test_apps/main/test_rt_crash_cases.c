/**
 * @file test_rt_crash_cases.c
 * @brief The deliberate crashes of the restart_tracker on-target run, and
 *        the three probes this app's own link puts around the crash note
 *        (main/CMakeLists.txt: -Wl,--wrap of three IDF functions). The
 *        component itself carries no test hook.
 *
 *        Test app only: ESP_ERROR_CHECK and panics are the point here.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_debug_helpers.h"
#include "esp_err.h"
#include "esp_private/cache_utils.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "restart_tracker.h"

#include "test_rt_cases.h"

#define RT_PROBE_MAGIC   0x52545042U /* "RTPB" */
#define RT_TASK_STACK    4096
#define RT_CHAIN_DEPTH   12
#define RT_CHAIN_UP      6           /* the frame whose saved caller SP goes */

typedef enum
{
    RT_INJECT_NONE = 0,
    RT_INJECT_FAULT,
    RT_INJECT_HANG,
} rt_inject_t;

/* ---- the probes ---------------------------------------------------------------
 * RTC memory: written in panic context, read by the next boot. */

typedef struct
{
    uint32_t magic;
    uint32_t t1; /* cycle count at stage B's second statement */
    uint32_t t2; /* cycle count at the reset call             */
} rt_probe_t;

static RTC_NOINIT_ATTR rt_probe_t s_probe;

static volatile uint32_t s_inject;      /* rt_inject_t, one shot */
static volatile int s_probe_core = -1;  /* the core that will crash */
static volatile bool s_measure;

BaseType_t __real_xPortInterruptedFromISRContext(void);
BaseType_t __wrap_xPortInterruptedFromISRContext(void);
TaskHandle_t __real_xTaskGetCurrentTaskHandleForCore(BaseType_t core);
TaskHandle_t __wrap_xTaskGetCurrentTaskHandleForCore(BaseType_t core);
void __real_esp_restart_noos(void) __attribute__((noreturn));
void __wrap_esp_restart_noos(void) __attribute__((noreturn));

/* IDF's register dump asks this too, so the value kept is the LAST call
   before the reset: stage B's second statement. */
BaseType_t __wrap_xPortInterruptedFromISRContext(void)
{
    if (s_measure && (int)esp_cpu_get_core_id() == s_probe_core)
    {
        s_probe.t1 = (uint32_t)esp_cpu_get_cycle_count();
    }

    return __real_xPortInterruptedFromISRContext();
}

/* Only stage B asks for a core's task while a panic is being handled: the
   place to make it fault or hang. */
TaskHandle_t __wrap_xTaskGetCurrentTaskHandleForCore(BaseType_t core)
{
    if (s_inject != RT_INJECT_NONE &&
        (int)esp_cpu_get_core_id() == s_probe_core)
    {
        uint32_t mode = s_inject;

        s_inject = RT_INJECT_NONE;

        if (mode == RT_INJECT_FAULT)
        {
            *(volatile uint32_t *)RT_BAD_ADDR_B = 0xDEADU;
        }

        while (mode == RT_INJECT_HANG)
        {
            /* until the RTC watchdog IDF arms for every panic resets us */
        }
    }

    return __real_xTaskGetCurrentTaskHandleForCore(core);
}

void __wrap_esp_restart_noos(void)
{
    s_probe.t2 = (uint32_t)esp_cpu_get_cycle_count();
    s_probe.magic = s_measure ? RT_PROBE_MAGIC : 0U;
    __real_esp_restart_noos();
}

bool rt_case_probe_take(uint32_t *stage_b_us)
{
    bool have = s_probe.magic == RT_PROBE_MAGIC;

    if (have)
    {
        *stage_b_us = (s_probe.t2 - s_probe.t1) /
                      esp_rom_get_cpu_ticks_per_us();
    }

    s_probe.magic = 0U;
    return have;
}

extern uint8_t _rtc_noinit_start[];
extern uint8_t _rtc_noinit_end[];

uint32_t rt_case_rtc_noinit_bytes(void)
{
    return (uint32_t)(_rtc_noinit_end - _rtc_noinit_start);
}

void rt_case_garbage_fill(void)
{
    for (volatile uint8_t *p = _rtc_noinit_start; p < _rtc_noinit_end; p++)
    {
        *p = 0xA5U;
    }
}

/* ---- the crash functions: noinline, so that a PC can be placed --------------- */

static volatile uint32_t s_sink;
static volatile int s_never;
static void (*volatile s_null_fn)(void); /* stays NULL */
static void (*volatile s_chain_fn)(int);

static void __attribute__((noinline)) rt_crash_store(void)
{
    volatile uint32_t *p = (volatile uint32_t *)RT_BAD_ADDR;

    *p = 0xDEADU;
    s_sink++; /* the store is not this function's last act */
}

static void __attribute__((noinline)) rt_crash_spin(void)
{
    for (;;)
    {
        s_sink++;
    }
}

static void __attribute__((noinline)) rt_crash_assert(void)
{
    assert(s_never == 1);
    s_sink++;
}

/* The fault is the fetch at address 0, before the callee's `entry`: the
   frame IDF gets is still this function's, so its return address names the
   CALLER of this function, not this function. */
static void __attribute__((noinline)) rt_crash_call_null(void)
{
    s_null_fn();
    s_sink++;
}

static void __attribute__((noinline)) rt_run_call_null(void)
{
    rt_crash_call_null();
    s_sink++;
}

static void IRAM_ATTR __attribute__((noinline)) rt_crash_cache_off(void)
{
    spi_flash_disable_interrupts_caches_and_other_cpu();
    *(volatile uint32_t *)RT_BAD_ADDR = 0xDEADU;

    for (;;)
    {
    }
}

static void __attribute__((noinline)) rt_chain_bottom(void)
{
    esp_backtrace_frame_t frame = { 0 };

    /* spills every register window of this task to its stack */
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);

    for (int i = 0; i < RT_CHAIN_UP; i++)
    {
        if (!esp_backtrace_get_next_frame(&frame))
        {
            break;
        }
    }

    /* under a frame's stack pointer lies the spilled stack pointer of its
       caller: make it one that is no stack at all */
    *(volatile uint32_t *)(frame.sp - 12U) = 0x00000001U;
    rt_crash_store();
    s_sink++;
}

static void __attribute__((noinline)) rt_chain(int depth)
{
    volatile uint32_t pad[4];

    pad[0] = (uint32_t)depth;

    if (depth > 0)
    {
        s_chain_fn(depth - 1);
    }
    else
    {
        rt_chain_bottom();
    }

    s_sink += pad[0]; /* no tail call: every level keeps its frame */
}

/* ---- the launchers -------------------------------------------------------------- */

static void store_task(void *arg)
{
    if (arg != NULL)
    {
        /* measure this one: stage B's time, by the probes above */
        s_probe_core = (int)esp_cpu_get_core_id();
        s_measure = true;
    }

    rt_crash_store();
    vTaskDelete(NULL);
}

static void inject_task(void *arg)
{
    portDISABLE_INTERRUPTS(); /* nothing between arming and the crash */
    s_probe_core = (int)esp_cpu_get_core_id();
    s_inject = (uint32_t)(uintptr_t)arg;
    rt_crash_store();
    vTaskDelete(NULL);
}

static void spin_task(void *arg)
{
    (void)arg;
    portDISABLE_INTERRUPTS();
    rt_crash_spin();
}

static StackType_t s_ovf_stack[RT_TASK_STACK];
static StaticTask_t s_ovf_tcb;

static void ovf_task(void *arg)
{
    (void)arg;

    /* what an overflow leaves behind: the fill pattern at the stack's limit
       is gone. FreeRTOS looks when this task is switched out. */
    memset(s_ovf_stack, 0, 32);

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static EXT_RAM_BSS_ATTR StackType_t s_psram_stack[RT_TASK_STACK];
static StaticTask_t s_psram_tcb;

static bool IRAM_ATTR on_alarm(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t *edata,
                               void *ctx)
{
    (void)timer;
    (void)edata;
    (void)ctx;
    rt_crash_store();
    return false;
}

static void run_isr(void)
{
    gptimer_handle_t timer = NULL;
    gptimer_config_t cfg =
    {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    gptimer_event_callbacks_t cbs = { .on_alarm = on_alarm };
    gptimer_alarm_config_t alarm = { .alarm_count = 100000 }; /* 100 ms */

    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &timer));
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    ESP_ERROR_CHECK(gptimer_start(timer));
}

const void *rt_case_fn(rt_step_t step)
{
    switch (step)
    {
        case RT_STEP_STORE:
        case RT_STEP_STORE_PSRAM:
        case RT_STEP_ISR:
        case RT_STEP_CORRUPT_CHAIN:
        case RT_STEP_STAGE_B_FAULT:
            return (const void *)rt_crash_store;
        case RT_STEP_ASSERT:
            return (const void *)rt_crash_assert;
        case RT_STEP_WDT_CPU0:
        case RT_STEP_WDT_CPU1:
            return (const void *)rt_crash_spin;
        case RT_STEP_CALL_NULL:
            return (const void *)rt_run_call_null;
        case RT_STEP_CACHE_OFF:
            return (const void *)rt_crash_cache_off;
        default:
            return NULL;
    }
}

void rt_case_run(rt_step_t step)
{
    switch (step)
    {
        case RT_STEP_STORE:
            xTaskCreatePinnedToCore(store_task, "rt_int", RT_TASK_STACK,
                                    (void *)1, 5, NULL, 0);
            break;
        case RT_STEP_STORE_PSRAM:
            xTaskCreateStaticPinnedToCore(store_task, "rt_psram",
                                          RT_TASK_STACK, NULL, 5,
                                          s_psram_stack, &s_psram_tcb, 0);
            break;
        case RT_STEP_ABORT:
            abort();
        case RT_STEP_ASSERT:
            rt_crash_assert();
            break;
        case RT_STEP_STACK_OVERFLOW:
            xTaskCreateStaticPinnedToCore(ovf_task, "rt_ovf", RT_TASK_STACK,
                                          NULL, 5, s_ovf_stack, &s_ovf_tcb,
                                          0);
            break;
        case RT_STEP_WDT_CPU0:
            xTaskCreatePinnedToCore(spin_task, "rt_spin", RT_TASK_STACK,
                                    NULL, 5, NULL, 0);
            break;
        case RT_STEP_WDT_CPU1:
            xTaskCreatePinnedToCore(spin_task, "rt_spin", RT_TASK_STACK,
                                    NULL, 5, NULL, 1);
            break;
        case RT_STEP_ISR:
            run_isr();
            break;
        case RT_STEP_CALL_NULL:
            rt_run_call_null();
            break;
        case RT_STEP_CACHE_OFF:
            rt_crash_cache_off();
            break;
        case RT_STEP_CORRUPT_CHAIN:
            s_chain_fn = rt_chain;
            rt_chain(RT_CHAIN_DEPTH);
            break;
        case RT_STEP_STAGE_B_FAULT:
            xTaskCreatePinnedToCore(inject_task, "rt_inj", RT_TASK_STACK,
                                    (void *)RT_INJECT_FAULT, 5, NULL, 0);
            break;
        case RT_STEP_STAGE_B_HANG:
            xTaskCreatePinnedToCore(inject_task, "rt_inj", RT_TASK_STACK,
                                    (void *)RT_INJECT_HANG, 5, NULL, 0);
            break;
        default:
            return; /* not a crash of this file */
    }

    /* the crash is a task's or an interrupt's: wait for it */
    vTaskDelay(pdMS_TO_TICKS(20000));
}
