# led_manager: host unit tests

The pure indication arbiter (`led_manager_policy.c`: no I2C, no FreeRTOS),
IDF `linux` target. Run via `.\test.ps1 host led_manager`. The chip layer
(`led_manager_aw2023.c`: register writes, the blink and breathe patterns,
their read-back) is hardware and is checked on the device (`led -d`,
`GET /api/led`, and the crash park of the firmware's `.\test.ps1 crashnote`,
which fails when the breathing pattern does not read back).

## What is covered (7 tests)

| Case | What it proves |
|---|---|
| empty is off | no indication set: nothing shows |
| idle shows | the IDLE indication alone is what the LED shows |
| higher priority wins | CRITICAL over IDLE, with its own mode and color |
| clear falls back | releasing a level shows the next occupied one down |
| replace in place | a second set at the same level replaces the first |
| bounds | a priority out of range, a NULL state and an unknown mode are refused |
| every mode is taken | off, solid, both blinks and breathe (2026-10-05) are modes like each other; the value after the last is refused |

## Expected result

```
7 Tests 0 Failures 0 Ignored
OK
```
