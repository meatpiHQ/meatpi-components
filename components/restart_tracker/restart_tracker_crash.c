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
 * @file restart_tracker_crash.c
 * @brief The crash note's target half: the store in RTC slow memory and the
 *        two link-time hooks around IDF's panic handler that fill it.
 *
 * IDF prints where a crash happened on the console and nowhere else. The
 * hooks (CMakeLists.txt: -Wl,--wrap=esp_panic_handler, --wrap=panic_restart)
 * keep the essentials for the next boot:
 *
 *   crash -> panic_handler()                 esp_system/port/panic_handler.c
 *     -> esp_panic_handler(info)   WRAPPED   stage A, then IDF's handler
 *          IDF prints the Guru Meditation text, as always
 *          -> panic_restart()      WRAPPED   stage B, then the reset
 *
 * Stage A stores ten words and follows no pointer but IDF's own `info` and
 * exception frame, so it cannot disturb the print that follows. Stage B
 * (task name, uptime, stack walk, the texts) runs when IDF has printed
 * everything: a fault there costs the tail of the note, never the console
 * text. Nothing here allocates, locks, logs, or writes flash or PSRAM.
 *
 * Both wrapped symbols are private to IDF. A rename fails the link; a call
 * that moved into the file of its target would stop the notes silently:
 * the on-target test app and `.\test.ps1 crashnote` are the guard, at every
 * IDF version bump. Xtensa only (the stack walk, the frame); other targets
 * build restart_tracker_crash_stub.c.
 */
#include "restart_tracker_private.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "xtensa/corebits.h"
#include "xtensa_context.h"

/* RTC slow memory: plain RAM beside the cache, kept over the CPU reset a
   panic ends in, never loaded or cleared at boot, part of no heap. Random
   after power-on: every note checks itself. */
static RTC_NOINIT_ATTR rt_crash_store_t s_store;

#define RT_HOOK_IDLE 0U /* no panic yet                                    */
#define RT_HOOK_HEAD 1U /* stage A ran (first panic of this run)           */
#define RT_HOOK_TAIL 2U /* stage B entered: a fault in it goes to the reset */

static volatile uint32_t s_hook;
static volatile bool s_nested; /* a panic inside the panic handler */
static volatile bool s_armed;  /* restart_tracker_init() has run   */

void __real_esp_panic_handler(panic_info_t *info);
void __real_panic_restart(void) __attribute__((noreturn));
void __wrap_esp_panic_handler(panic_info_t *info);
void __wrap_panic_restart(void) __attribute__((noreturn));

/* ---- stage A: before IDF prints ----------------------------------------------- */

static uint8_t kind_of(const panic_info_t *info)
{
    if (g_panic_abort)
    {
        /* IDF's own handler makes the same override a moment later */
        return RESTART_TRACKER_CRASH_ABORT;
    }

    switch (info->exception)
    {
        case PANIC_EXCEPTION_IWDT:  return RESTART_TRACKER_CRASH_INT_WDT;
        case PANIC_EXCEPTION_TWDT:  return RESTART_TRACKER_CRASH_TASK_WDT;
        case PANIC_EXCEPTION_DEBUG: return RESTART_TRACKER_CRASH_DEBUG;
        case PANIC_EXCEPTION_ABORT: return RESTART_TRACKER_CRASH_ABORT;
        default:                    return RESTART_TRACKER_CRASH_EXCEPTION;
    }
}

static void stage_a(const panic_info_t *info)
{
    rt_crash_note_t *note = &s_store.pending;
    const XtExcFrame *frame = (const XtExcFrame *)info->frame;

    memset(note, 0, sizeof(*note));
    note->kind = kind_of(info);
    note->core = (uint8_t)info->core;
    note->head_flags = info->pseudo_excause ? RT_CRASH_HF_PSEUDO : 0U;

    if (frame != NULL)
    {
        note->cause = (uint32_t)frame->exccause;
        note->pc = (uint32_t)frame->pc;
        note->excvaddr = (uint32_t)frame->excvaddr;
        note->sp = (uint32_t)frame->a1;
        note->ra = (uint32_t)frame->a0;
    }

    note->reason_ptr = (uint32_t)(uintptr_t)info->reason;
    note->magic = RT_CRASH_NOTE_MAGIC;
    note->head_crc = rt_crash_head_crc(note);
}

/* ---- stage B: after IDF printed everything ------------------------------------- */

/** IDF's `Backtrace:` walk (esp_backtrace_print_from_frame) into an array:
 *  the same first-frame check, the same step, the same PC processing. */
static uint8_t walk(uint32_t pc, uint32_t sp, uint32_t next_pc,
                    bool first_pc_may_be_bad, uint32_t *out, uint8_t cap,
                    bool *corrupt, bool *more)
{
    esp_backtrace_frame_t frame =
    {
        .pc = pc,
        .sp = sp,
        .next_pc = next_pc,
        .exc_frame = NULL,
    };
    uint8_t n = 0;

    out[n++] = esp_cpu_process_stack_pc(frame.pc);

    /* an InstrFetchProhibited PC is bad by definition: IDF goes on from it */
    bool bad = !(esp_stack_ptr_is_sane(frame.sp) &&
                 (esp_ptr_executable(
                      (void *)esp_cpu_process_stack_pc(frame.pc)) ||
                  first_pc_may_be_bad));

    while (n < cap && frame.next_pc != 0U && !bad)
    {
        if (!esp_backtrace_get_next_frame(&frame))
        {
            bad = true; /* recorded all the same, as IDF prints it */
        }

        out[n++] = esp_cpu_process_stack_pc(frame.pc);
    }

    *corrupt = bad;
    *more = !bad && frame.next_pc != 0U;
    return n;
}

static bool readable(const char *p)
{
    return esp_ptr_in_drom(p) || esp_ptr_byte_accessible(p);
}

/** Up to `cap - 1` characters of a string IDF has just printed; every
 *  address is checked before it is read. Returns true when it was longer. */
static bool copy_text(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (src == NULL)
    {
        return false;
    }

    for (; i + 1U < cap; i++)
    {
        if (!readable(src + i) || src[i] == '\0')
        {
            return false;
        }

        dst[i] = src[i];
    }

    return readable(src + i) && src[i] != '\0';
}

static void copy_task_name(char *dst, uint32_t core)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandleForCore((BaseType_t)core);

    if (task == NULL || !esp_ptr_byte_accessible(task))
    {
        return; /* before the scheduler, or a block that is not memory */
    }

    const char *name = pcTaskGetName(task);

    if (name == NULL || !esp_ptr_byte_accessible(name) ||
        !esp_ptr_byte_accessible(name + RT_CRASH_TASK_LEN - 1))
    {
        return;
    }

    for (int i = 0; i < RT_CRASH_TASK_LEN && name[i] != '\0'; i++)
    {
        dst[i] = name[i];
    }
}

static void stage_b(void)
{
    rt_crash_note_t *note = &s_store.pending;
    uint8_t flags = 0;
    bool corrupt = false;
    bool more = false;

    /* a cache error can report core -1: FreeRTOS asserts on that */
    uint32_t core = (note->core < SOC_CPU_CORES_NUM)
                        ? note->core : (uint32_t)esp_cpu_get_core_id();

    if (s_armed)
    {
        note->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    }
    else
    {
        flags |= RT_CRASH_TF_EARLY;
    }

    if (xPortInterruptedFromISRContext())
    {
        flags |= RT_CRASH_TF_IN_ISR;
    }

    if (s_nested)
    {
        flags |= RT_CRASH_TF_NESTED;
    }

    /* the DRAM array IDF fills at startup and prints in every panic */
    memcpy(note->elf, esp_app_get_elf_sha256_str(),
           (CONFIG_APP_RETRIEVE_LEN_ELF_SHA < RT_CRASH_ELF_LEN)
               ? CONFIG_APP_RETRIEVE_LEN_ELF_SHA : RT_CRASH_ELF_LEN);
    copy_task_name(note->task, core);

    note->bt_len = walk(note->pc, note->sp, note->ra,
                        note->cause == EXCCAUSE_INSTR_PROHIBITED, note->bt,
                        RT_CRASH_BT_LEN, &corrupt, &more);

    if (corrupt)
    {
        flags |= RT_CRASH_TF_BT_CORRUPT;
    }

    if (more)
    {
        flags |= RT_CRASH_TF_BT_MORE;
    }

#if SOC_CPU_CORES_NUM > 1
    /* IDF saves the other core's frame only when both entered the handler:
       the interrupt watchdog and the cache error */
    uint32_t other = core ^ 1U;
    const XtExcFrame *frame = (const XtExcFrame *)g_exc_frames[other];

    if (frame != NULL && esp_ptr_byte_accessible(frame))
    {
        note->bt2_len = walk((uint32_t)frame->pc, (uint32_t)frame->a1,
                             (uint32_t)frame->a0,
                             frame->exccause == EXCCAUSE_INSTR_PROHIBITED,
                             note->bt2, RT_CRASH_BT2_LEN, &corrupt, &more);
        note->bt2_core = (uint8_t)other;
    }
#endif

    if (note->kind == RESTART_TRACKER_CRASH_ABORT)
    {
        /* no reason: IDF's would read IllegalInstruction, an artefact of
           how abort() enters the handler. The message is the abort's. */
        if (copy_text(note->text, sizeof(note->text), g_panic_abort_details))
        {
            flags |= RT_CRASH_TF_TEXT_CUT;
        }
    }
    else
    {
        (void)copy_text(note->reason, sizeof(note->reason),
                        (const char *)(uintptr_t)note->reason_ptr);
    }

    note->tail_flags = flags;
    note->tail_crc = rt_crash_tail_crc(note);
}

/* ---- the hooks -------------------------------------------------------------------- */

/** IDF calls this with the panic info filled, before it prints anything. */
void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (esp_cpu_compare_and_set(&s_hook, RT_HOOK_IDLE, RT_HOOK_HEAD))
    {
        stage_a(info);
    }
    else
    {
        s_nested = true; /* the first panic keeps its note */
    }

    __real_esp_panic_handler(info);
}

/** IDF calls this when everything is printed, to reset the chip. */
void __wrap_panic_restart(void)
{
    if (esp_cpu_compare_and_set(&s_hook, RT_HOOK_HEAD, RT_HOOK_TAIL))
    {
        stage_b();
    }

    __real_panic_restart();
}

/* ---- the next boot, and the readers ------------------------------------------------ */

const rt_crash_note_t *rt_crash_boot_collect(uint32_t sequence, uint32_t slot,
                                             bool fresh_history)
{
    const rt_crash_note_t *note =
        rt_crash_collect(&s_store, sequence, slot, fresh_history);

    s_armed = true;
    return note;
}

rt_brake_state_t *rt_crash_brake_state(void)
{
    /* the same RTC memory as the notes, and never written by the hooks */
    return s_armed ? &s_store.brake : NULL;
}

esp_err_t restart_tracker_get_crash(uint32_t sequence,
                                    restart_tracker_crash_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* no lock: after init the kept notes change only at the next boot */
    const rt_crash_note_t *note = rt_crash_find(&s_store, sequence);

    if (note == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    rt_crash_export(note, out);
    return ESP_OK;
}
