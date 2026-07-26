"""On-target tests for obd_chip against the LIVE bench (chip + ECU simulator;
see BENCH.md in this directory for the bench). The marker sequence mirrors
test_apps/README.md.

Run (hardware):
    pytest --target esp32s3 pytest_obd_chip.py
"""
import pytest


@pytest.mark.esp32s3
def test_obd_chip(dut):
    dut.expect_exact("INIT ok=1")
    dut.expect("START ok=1 ready_pin=\\d")

    # request -> response against the real chip
    dut.expect_exact("REQ ati_ok=1 elm=1")
    dut.expect("VERSION ok=1 mic=1 resp=MIC3624 V2\\.3\\.\\d+")

    # ECU simulator: standard PID + the canonical multi-frame VIN
    dut.expect_exact("PID0100 ok=1 has41=1")
    dut.expect("VIN ok=1 lines=[2-9]\\d* has4902=1")

    # broadcast fan-out + slow-consumer drop policy
    dut.expect("FANOUT chunks=\\d+ gt0=1")
    dut.expect("DROPS tiny=\\d+ gt0=1 probe_intact=1")

    # claim arbitration: MONITOR blocks commands, release restores
    dut.expect_exact("CLAIM blocked=1")
    dut.expect_exact("CLAIM released_ok=1")

    # monitor-class handling: classified, refused by request(), and a real
    # ATMA session stopped with the SPACE byte
    dut.expect_exact("MONITOR classified=1 refused=1")
    dut.expect_exact("ATMA stopped_ok=1")

    # vendor firmware staged for the (manual) update procedure
    dut.expect("FWSTAGE ok=1 bytes=\\d+")
    dut.expect_exact("BRIDGE READY")
    dut.expect_exact("TEST DONE")
