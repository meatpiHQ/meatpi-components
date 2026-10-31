# ble_manager: host unit suite

Compiles ONLY the pure layer (`ble_manager_pack.c`: no BT stack) on the
IDF **linux** target. Runs on the bench Pi via `.\test.ps1 host`.

## What is covered (16 tests)

- Legacy TX packing math: packets-needed round-up (partial packet costs
  one), zero-max guard.
- Fill/flush state machine: partial fill, fill across two input chunks with
  correct leftover, exact-boundary fill, byte order preserved.
- Identity derivations existing tools depend on: device name
  `WiC_<id>`, serial = name + 7 (incl. the too-short guard), TX-power
  clamp to the S3's -12..+9 3 dB steps.
- The settings words of v4: the connection profile, the PHY mask, the
  advertising mode; the stream channel's OUT-mode picker.
- The Bluetooth memory profile of v5 (`test_ram.c`, 2026-10-31): the
  profile words (unknown = lean, the default), the profile-to-controller
  mapping (full keeps the build's values, lean is the measured set, custom
  copies and clamps to the controller's ranges, 0 keeps scanning off), the
  validation rule (lean refuses extended advertising and a PHY other than
  1M with a reason naming the way out, a custom scan cache of 1..9 is
  refused, the other profiles keep every combination) and the v4 to v5
  migration verdict (a BLE 5 choice keeps Full, the 4.2 defaults take the
  schema default).

## Expected result

```
16 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-10-31 on rpi001 (`.\test.ps1 host ble_manager`).
