# ble_manager — host unit suite

Compiles ONLY the pure layer (`ble_manager_pack.c` — no BT stack) on the
IDF **linux** target. Runs on the bench Pi via `.\test.ps1 host`.

## What is covered (6 tests)

- Legacy TX packing math: packets-needed round-up (partial packet costs
  one), zero-max guard.
- Fill/flush state machine: partial fill, fill across two input chunks with
  correct leftover, exact-boundary fill, byte order preserved.
- Identity derivations existing tools depend on: device name
  `WiC_<id>`, serial = name + 7 (incl. the too-short guard), TX-power
  clamp to the S3's −12..+9 3 dB steps.

## Expected result

```
6 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-07-04 on rpi001.
