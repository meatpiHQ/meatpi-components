"""On-target tests for api_http: the full /api surface against a real
composed stack (log/restart/dev_status/filesystem/settings/http server) on
the lwIP loopback. TWO-PHASE: the suite reboots the DUT once through
POST /api/settings/submit and verifies persistence + restart forensics
after the reboot. Builds against the MAIN firmware's partition table.

Run (hardware):
    pytest --target esp32s3 pytest_api_http.py
"""
import pytest


@pytest.mark.esp32s3
def test_api_http(dut):
    dut.expect_exact("INIT ok=1")
    dut.expect_exact("PHASE 1")

    # settings surface: list, redacted GET, schema, 404, validation error
    dut.expect_exact("SETTINGS-LIST ok=1 has_api_test=1")
    dut.expect_exact("SETTINGS-GET ok=1 degraded0=1 redacted=1")
    dut.expect_exact("SCHEMA ok=1 has_props=1")
    dut.expect_exact("SETTINGS-404 ok=1")
    dut.expect_exact("PUT-BAD rejected=1 has_err=1")

    # status + restart history
    dut.expect_exact("STATUS ok=1 awake=1 uptime=1 boots=1")
    dut.expect_exact("HISTORY ok=1 has_records=1")

    # log-manager runtime knobs + crash-ring dump
    dut.expect_exact("LOGS-STATUS ok=1 has_console=1 has_ring=1")
    dut.expect_exact("LOGS-LEVEL ok=1")
    dut.expect_exact("LOGS-LEVEL-BAD rejected=1")
    dut.expect_exact("LOGS-SINK-404 ok=1")
    dut.expect_exact("LOGS-RING ok=1 bytes_gt0=1")

    # read-only filesystem browse
    dut.expect_exact("FS-LIST ok=1 has_probe=1")
    dut.expect_exact("FS-LIST-BAD rejected=1")
    dut.expect_exact("FS-INFO ok=1 has_total=1")

    # the real change: PUT changed=true, "" password keeps stored (dedup)
    dut.expect_exact("PUT-OK ok=1 changed=1")
    dut.expect_exact("PUT-KEEP noop=1")

    # submit with changes: {"reboot":true} then the device REBOOTS
    dut.expect_exact("SUBMIT ok=1 reboot=1")

    # phase 2, after the reboot
    dut.expect_exact("PHASE 2 (after submit reboot)")
    dut.expect_exact("PHASE2-PERSISTED ok=1 value7=1")
    dut.expect_exact("PHASE2-HISTORY ok=1 planned=1 reason=1 source=1")
    dut.expect_exact("PHASE2-NOOP-SUBMIT ok=1 noreboot=1")

    dut.expect_exact("TEST DONE")
