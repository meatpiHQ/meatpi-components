"""On-target tests for restart_tracker: PSRAM .noinit survival across a real
esp_restart(), planned-intent consumption, reset-reason classification, and
the crash note (one deliberate crash per boot, the next boot reports what
the tracker filed), and on the same crashes the crash-loop brake's count
and the stored crash report's write budget. Builds against the MAIN
firmware's partition table (standard rev 2.1) and stores the report in the
DUT's own NVS, which it clears of it at the end.

Run (hardware):
    pytest --target esp32s3 pytest_restart_tracker.py

The bench runs the same app through `.\\test.ps1 target restart_tracker`,
whose checker (firmware repo, tools/testbench/system/rt_target_check.py)
also compares every note with IDF's own panic text of the same crash. This
file keeps the strict marker sequence for a pytest-embedded rig.
"""
import pytest

STEPS = ["planned_restart", "store_in_task", "store_on_psram_stack", "abort",
         "assert", "stack_overflow", "int_wdt_cpu0", "int_wdt_cpu1",
         "store_in_isr", "call_null", "store_cache_off", "corrupt_chain",
         "stage_b_fault", "stage_b_hang", "garbage_store"]


@pytest.mark.esp32s3
def test_restart_tracker(dut):
    # Step 0: the first boot after flashing announces a planned restart.
    dut.expect_exact("RT STEP 0 planned_restart")
    dut.expect_exact("PHASE1 marking planned restart and rebooting")

    # After the real reboot: the intent was consumed and recorded.
    dut.expect_exact(
        "PHASE2 planned=1 reason=user_request source=console flags=0xC0FFEE")

    # History survived the warm reset (boot_count kept counting).
    dut.expect("SURVIVED boots=\\d+ unexpected=0 history_kept=1")

    # esp_restart() shows up as a software reset and is not "unexpected".
    dut.expect_exact("CLASSIFY reset=software unexpected_count=0")

    # Every step, the crashes included, is judged by the boot after it. The
    # hang step waits ten seconds for the RTC watchdog.
    for n, name in enumerate(STEPS):
        dut.expect_exact(f"RT RESULT step={n} {name} ok", timeout=40)

    # The run takes its crash report out of NVS again.
    dut.expect_exact("RT CLEANUP report_cleared=1")
    dut.expect_exact(f"RT SUMMARY ok={len(STEPS)} of={len(STEPS)}")
    dut.expect_exact("TEST DONE")
