# script_engine host tests

Unity suite for `se_script_name_ok` (script_engine_name.c) — the
script-name guard on the `/data/scripts` load path and the `script.run`
event action. Security-relevant (a bad name reaches a filesystem open), so
it must reject path traversal (`..`), separators (`/ \`), injection chars,
non-ASCII, empty/over-long, and accept only `[A-Za-z0-9_.-]` ≤ 40 chars.

The rest of script_engine (Berry VM host, run-file task, HTTP, bindings) is
integration glue over the vendored Berry interpreter and the filesystem —
covered on the bench, not host-testable in isolation. The event-manager
`{"script":"name"}` sugar rewrite is covered by event_manager's own host
suite (`test_rules_parse_script_sugar`).

Run: `idf.py --preview set-target linux && idf.py build && ./build/*.elf`
or `./test.ps1 host script_engine`.
