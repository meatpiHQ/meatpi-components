# can_selftest: host unit tests

The pure half only (`can_selftest_parse.c`: no CAN, no UART). Run via
`.\test.ps1 host can_selftest`.

## What is covered (9 tests)

| Case | What it proves |
|---|---|
| ours, headers on | `7EF 04 41 0C 1A F8` is the ESP32's reply, 1726 rpm decoded |
| ours after other ECUs | a live vehicle's 7E8/7E9 lines first, ours still found |
| other ECUs only | 7E8 alone = `other` (the ESP32's reply never reached the chip), no rpm |
| NO DATA and the errors | `NO DATA` = no_data; `CAN ERROR` / `BUS ERROR` = can_error; `?` and `UNABLE TO CONNECT` = error |
| empty and noise | empty, NULL and `SEARCHING...` = none |
| wrong PID is not ours | a 7EF line of another PID is not the reply |
| CAN ERROR wins | `NO DATA` then `CAN ERROR` classifies as can_error |
| rpm edges | 00 00 = 0, FF FF = 16383 |
| verdicts | PASS only when the ESP32 heard and the chip printed ours; the four "where to look" lines by (esp_heard, reply) |

## Expected result

```
9 Tests 0 Failures 0 Ignored
OK
```
