# bridge_manager: host unit suite

Compiles the pure config layer (`bridge_manager_config.c`) plus a
line-splitter test codec that exercises the `bridge_translator_t` CONTRACT:
no FreeRTOS runtime use, IDF **linux** target. Runs via `.\test.ps1 host`.

## What is covered (15 tests)

Config: parse defaults (`translator:"raw"`, disabled), valid sets, an
unregistered endpoint name ACCEPTED at a runtime PUT (2026-10-29: the jack
of a server or channel created in the same submit registers at the next
boot, and a row stranded by a server's removal must not refuse every later
save; the bridge degrades alone at start) while two enabled bridges on
such a name are still refused (it counts as single-stream), unknown
translator rejected with the offending name in the error, `raw` always
known, `a == b` rejected, the **single-consumer rule** (+ the
`multi_consumer` fan-out exemption and its boot-lenient skip, 2026-07-18)
(endpoint in
two ENABLED bridges rejected; disabled duplicates allowed), duplicate
bridge names rejected.

Translator contract (the ctx/sink plumbing every real codec relies on):
ctx fits the manager's 256 B pool slot; one input chunk → many sink outputs;
a frame FRAGMENTED across three chunks reassembles (zero outputs mid-frame:
the chunk-boundary bug class); two ctx instances (the pump's a2b/b2a) are
fully isolated.

## Expected result

```
15 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-10-29 on rpi001 (the suite had grown to 14 since the
12 of 2026-07-03; the unknown-endpoint case became two tests on 2026-10-29).
`.\test.ps1 host bridge_manager` syncs the components tree it resolves and
prints it as `components: <path>` at the start of the run (since 2026-10-29
the dev layout's live tree by default, `MEATPI_COMPONENTS_PATH` to
override): check that line, the 02:21 run that day synced a stale clone.
