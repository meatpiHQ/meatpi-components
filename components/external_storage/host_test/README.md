# external_storage — host unit suite

Compiles ONLY the pure card-detect debouncer (`es_detect.c` — no
GPIO/SDMMC) on the IDF **linux** target. Runs via `.\test.ps1 host`.

## What is covered (3 tests)

Glitches shorter than the 4-sample threshold never surface (1–3 sample
bursts); a clean insert and remove each fire the change edge exactly once
(steady state never re-fires); alternating "seating wobble" samples never
accumulate and the state flips exactly once after it settles.

## Expected result

```
3 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-07-04 on rpi001.
