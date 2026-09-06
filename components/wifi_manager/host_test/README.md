# wifi_manager — host unit tests

Pure selection/failure-memory suite (`wifi_manager_select.c` only — no esp_wifi),
IDF `linux` target. Run via `.\test.ps1 host` or manually per
`components/TESTBENCH.md` §4.

## What is covered (23 tests)

| Case | What it proves |
|---|---|
| scan prefers primary | primary wins whenever visible, regardless of scan order |
| falls back in priority order | primary absent → fallback1 beats fallback2 |
| skips failed candidate | an entry whose last 3 attempts failed yields to a visible clean fallback |
| all failed keeps trying, round-robin | never defer: when every visible entry failed they alternate every cycle |
| clean alternative always first | a clean entry beats a failed one every cycle, for as long as it takes |
| nothing present returns none | no configured network visible → defer, don't guess |
| deprioritised after threshold + fade | 3 strikes deprioritise; the memory fades after `WM_FAIL_MEMORY_MS` |
| faded streak starts over | old strikes do not add up with a new one after the fade |
| success clears failures | one good connect resets an entry's strikes |
| deprioritise at once | a failed roam trial needs one strike; later strikes keep counting |
| sequential rotates and wraps | no-scan path round-robins 0→1→2→0 |
| sequential skips failed | rotation hops over entries that failed lately |
| sequential all failed rotates anyway | rotation degrades to "try them all in turn", never to silence |
| single network keeps trying | one SSID, rejected: returned every cycle (the drive-home case) |
| roam better available | migrate to a visible clean preferred entry; not to one that rejected us until the memory fades |
| duplicate SSID entries independent | same name, two passwords: strikes on one never block the other |
| roam never to same SSID | the same name as the working connection is the same AP: stay |
| parse / netmask / backoff / AP-client pause | unchanged helpers |

## Expected result

```
23 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-07-02 on rpi001. The radio/event integration of the
same logic (real auth failures, real reconnects) is covered by
`../test_apps_hil` on the bench.
