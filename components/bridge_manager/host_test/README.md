# bridge_manager — host unit suite

Compiles the pure config layer (`bridge_manager_config.c`) plus a
line-splitter test codec that exercises the `bridge_translator_t` CONTRACT —
no FreeRTOS runtime use, IDF **linux** target. Runs via `.\test.ps1 host`.

## What is covered (12 tests)

Config: parse defaults (`translator:"raw"`, disabled), valid sets, unknown
endpoint/translator rejected with the offending name in the error, `raw`
always known, `a == b` rejected, the **single-consumer rule** (+ the
`multi_consumer` fan-out exemption and its boot-lenient skip, 2026-07-18)
(endpoint in
two ENABLED bridges rejected; disabled duplicates allowed), duplicate
bridge names rejected.

Translator contract (the ctx/sink plumbing every real codec relies on):
ctx fits the manager's 256 B pool slot; one input chunk → many sink outputs;
a frame FRAGMENTED across three chunks reassembles (zero outputs mid-frame —
the chunk-boundary bug class); two ctx instances (the pump's a2b/b2a) are
fully isolated.

## Expected result

```
12 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-07-03 on rpi001.
