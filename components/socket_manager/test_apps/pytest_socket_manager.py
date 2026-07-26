"""On-target tests for socket_manager: TCP aggregate + UDP last-peer
semantics, max_clients, dead-client reaping — all on the lwIP loopback.
Builds against the MAIN firmware's partition table + sdkconfig.

Run (hardware):
    pytest --target esp32s3 pytest_socket_manager.py
"""
import pytest


@pytest.mark.esp32s3
def test_socket_manager(dut):
    dut.expect_exact("INIT ok=1")
    dut.expect_exact("CFG ok=1 err=''")
    dut.expect_exact("START ok=1")
    dut.expect_exact("SUB ok=1 dup_rejected=1")

    dut.expect_exact("TCP-RX ok=1 match=1")
    dut.expect_exact("TCP-TX ok=1 match=1")
    dut.expect_exact("FANOUT ok=1")
    dut.expect_exact("MERGE ok=1")
    dut.expect_exact("MAXCLIENTS closed=1 refused=1")
    dut.expect_exact("REAP ok=1 clients=1")

    dut.expect_exact("UDP-NOPEER refused=1")
    dut.expect_exact("UDP-RX ok=1")
    dut.expect_exact("UDP-LASTPEER ok=1")

    dut.expect_exact("TEST DONE")
