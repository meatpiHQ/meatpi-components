"""On-target tests for bridge_manager: stub pump (ordered/lossless +
ceiling), producer-side overflow drop-and-count, and the socket end-to-end
echo leg on the lwIP loopback. Builds against the MAIN firmware's partition
table + sdkconfig.

Run (hardware):
    pytest --target esp32s3 pytest_bridge_manager.py
"""
import pytest


@pytest.mark.esp32s3
def test_bridge_manager(dut):
    dut.expect_exact("INIT ok=1")
    dut.expect_exact("ENDPOINTS ok=1 dup_rejected=1")
    dut.expect_exact("CFG ok=1 err=''")
    dut.expect_exact("CFG-BAD rejected=1")
    dut.expect_exact("START ok=1")

    dut.expect_exact("PUMP ok=1 in_order=1 drops=0")
    dut.expect("PUMP-CEILING chunks_per_s=\\d+ kbytes_per_s=\\d+")
    dut.expect_exact("OVERFLOW drops_gt0=1 pump_alive=1 accounted=1")
    dut.expect_exact("E2E ok=1 match=1")

    dut.expect_exact("STOP ok=1")
    dut.expect_exact("TEST DONE")
