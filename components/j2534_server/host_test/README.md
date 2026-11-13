# J2534 host tests

From the firmware checkout: `.\test.ps1 host j2534_server`.
Expected: `31 Tests 0 Failures 0 Ignored`.

The suite covers header/message codecs and bounds, filter matching, stream
framing, ACK vectors, TCP exposure, baud checks, ISO15765 layouts and
indications, J1850 request/response helpers, and the settings migration.

The production READ_VBATT helper runs against a fake battery owner: voltage
conversion and rounding, a valid zero, refusal without an open device, no
sample, NaN/infinity, negative and out-of-range readings. Failures leave the
result untouched. No ADC hardware is emulated here. The device and real DLL
half is the firmware's `.\test.ps1 j2534vbat`.
