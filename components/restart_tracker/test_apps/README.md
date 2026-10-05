# restart_tracker: on-target test app

Self-driving, **one step per boot**, the step number kept in PSRAM
`.noinit`. Builds against the main partition table (standard rev 2.1), and
only `main` and what it requires (`MINIMAL_BUILD`): a first build is about
two minutes. Run:

```powershell
.\test.ps1 target restart_tracker
```

It flashes the app, then `tools/testbench/system/rt_target_check.py`
(firmware repo) resets the DUT, reads the console to `TEST DONE` and gives
`RT TARGET PASS (<n> checks)`. The run crashes the device 13 times on
purpose, about one minute in all; the last but one step waits ten seconds
for the RTC watchdog. Flash the main firmware back afterwards.

## What is covered

| Step | What the app does | What the next boot must find |
|---|---|---|
| 0 `planned_restart` | `restart_tracker_restart(USER_REQUEST, CONSOLE, 0xC0FFEE)` | the PSRAM state survived a genuine `esp_restart()`; the intent recorded (reason / source / flags); reset `software`, not counted as unexpected; no crash note |
| 1 `store_in_task` | a store to 0x0000BAD0 in task `rt_int` | note: `exception`, `StoreProhibited`, that address, the task, the PC inside the crash function |
| 2 `store_on_psram_stack` | the same in task `rt_psram`, its stack in PSRAM | the same: the stack walk goes through PSRAM |
| 3 `abort` | `abort()` | `abort`, text `abort() was called at PC 0x...` |
| 4 `assert` | a failing `assert` | text `assert failed: rt_crash_assert ...`, the function in the backtrace |
| 5 `stack_overflow` | the fill pattern at a task's stack limit overwritten | text `... stack overflow in task rt_ovf ...` |
| 6 `int_wdt_cpu0` | interrupts off and spin, core 0 | reset `interrupt_wdt`, kind `int_wdt`, `Interrupt wdt timeout on CPU0`, the PC in the spin function, the other core's frames |
| 7 `int_wdt_cpu1` | the same on core 1 | core 1, `... on CPU1` |
| 8 `store_in_isr` | the store inside a timer interrupt | `in_isr` |
| 9 `call_null` | a call through a NULL function pointer | `InstrFetchProhibited`, PC 0, the callers still listed |
| 10 `store_cache_off` | the store with the flash cache disabled (IRAM function) | the note is there all the same |
| 11 `corrupt_chain` | a spilled stack pointer overwritten twelve calls deep, then the store | `backtrace_corrupt`, one panic, not two; the notes of the two boots before are still filed |
| 12 `stage_b_fault` | the note's own stage B made to fault | the note is incomplete and is the ORIGINAL crash's (0x0000BAD0, not the injected 0x0000BAD4); the first panic text is whole on the console |
| 13 `stage_b_hang` | stage B made to hang | reset `wdt` after ten seconds; measured: the head-only note and the older notes survive that reset |
| 14 `garbage_store` | `.rtc_noinit` filled with 0xA5, planned restart | no note, the old ones gone, nothing crashes |

After every crash step the tracker's unexpected-reset count must have grown
by exactly one.

**The crash-loop brake and the stored crash report** (2026-10-05) ride on
the same run: no run of this app lives a second, so every crash is a quick
one, and every crash is another one than the crash before. Each boot prints
`RT BRAKE` and `RT REPORT` and checks:

| When | What must hold |
|---|---|
| step 0 starts | the app marks the run settled: no streak, a full report budget, whatever the firmware left in RTC memory |
| after step 0 (planned restart) | the brake at rest: verdict `normal`, streak 0, budget 4 |
| after crash step n (1 to 13) | streak n; verdict `normal` for n = 1, 2 and `park` from n = 3 (the app parks nothing: it only reads the verdict) |
| after crash steps 1 to 4 | the report in NVS is this crash (kind, pc, text, task, call chain), the budget went down by one, `parked` from the third |
| after crash steps 5 to 13 | the budget is spent: nothing is stored, the report stays step 4's |
| after step 14 (the store filled with garbage) | the count went with the store: streak 0, budget 4 |
| `TEST DONE` | the app clears the report (`RT CLEANUP report_cleared=1`): the firmware that is flashed back must not show this run's crashes as the device's |

The app opens the DUT's own NVS partition (`RT NVS init=ESP_OK`) and never
erases it: it holds the firmware's calibration, bonds and fault codes.

`rt_target_check.py` adds what only the PC sees: every note against IDF's
own panic text of the same crash (the PCs of the `Backtrace:` line, the
register dump's PC and EXCVADDR, the `CORRUPTED` / `CONTINUES` marks, the
other core's frames), the number of panic texts a step printed, and one
`previous run crashed` log line per filed note. It also reads the `RT BRAKE`
lines as a whole: the streak counted every one of the 13 crashes, the
verdict was `park` from the third, the report budget went 3, 2, 1, 0 and
stayed 0, NVS was there, the report was cleared.

## How stage B is made to fault

The component carries no test hook. This app's own link wraps three IDF
functions (`main/CMakeLists.txt`): `xTaskGetCurrentTaskHandleForCore()`,
which only stage B calls while a panic is handled (the place to fault or
hang), `xPortInterruptedFromISRContext()` and `esp_restart_noos()` (cycle
counts: the time stage B takes, `RT MEASURE stage_b_us`).

## Expected result: serial markers

```
BOOT count=1 seq=1 reason=poweron planned=0
RT STEP 0 planned_restart
PHASE1 marking planned restart and rebooting
...
RT STEP 1 store_in_task
RT NOTE step=0 found=0 reset=software planned=1
PHASE2 planned=1 reason=user_request source=console flags=0xC0FFEE
SURVIVED boots=2 unexpected=0 history_kept=1
CLASSIFY reset=software unexpected_count=0
RT RESULT step=0 planned_restart ok
Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.
...
RT STEP 2 store_on_psram_stack
RT NOTE step=1 found=1 reset=panic planned=0 kind=exception cause=29 pc=0x4200bc00 excvaddr=0x0000bad0 core=0 complete=1 ... task="rt_int" reason="StoreProhibited" text=""
RT NOTEBT step=1 0x4200bbfd 0x4200bcf4 0x4037eee9
RT BRAKE step=1 verdict=normal streak=1 parks=0 budget=3 budget_before=4
RT REPORT step=1 found=1 pc=0x4200bc00 streak=1 parked=0
RT RESULT step=1 store_in_task ok
...
RT CLEANUP report_cleared=1
RT SUMMARY ok=15 of=15
TEST DONE
```

A power-on or the flasher's reset starts the run at step 0; any other reset
is the run's own next boot. Last verified: 2026-10-05 on WiCAN Pro, IDF
v6.0.2 (`RT TARGET PASS`, 79 checks; stage B 242 us).
