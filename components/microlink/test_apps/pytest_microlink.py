"""Real-kernel ownership across a timed-out MicroLink stop and destroy."""
import pytest


@pytest.mark.esp32s3
def test_timed_out_task_stays_owned(dut):
    dut.expect_exact("MICROLINK TEST: stop must retain a live task", timeout=15)
    dut.expect_exact("MICROLINK TEST: a second stop through destroy", timeout=40)
    dut.expect_exact("1 Tests 0 Failures 0 Ignored", timeout=40)
    dut.expect_exact("TEST DONE")
