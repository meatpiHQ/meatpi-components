# event_manager — host unit tests

Pure modules only (`event_manager_rules.c`, `event_manager_template.c`
with `EM_HOST_TEST`). Run via `.\test.ps1 host event_manager`.

## What is covered (8 tests)

| Case | What it proves |
|---|---|
| rules parse happy | name/on/match/when/do/with/cooldown all land typed |
| rules parse rejects | selector without a dot, unknown op, ordered op with string operand, duplicate names; empty/null = valid |
| match + every op | typed equality pre-filter; ==/!=/>/>=/</<= over the same event |
| contains + string eq | substring + exact string conditions |
| **changed semantics** | first occurrence counts as changed; same value = false; per-rule slot tracks the previous match-passing value |
| cooldown arithmetic | 64-bit µs windows (fake clock): inside = suppressed, boundary+1 = allowed; 0 = always |
| template render | event kv (f64/str), ${ts} builtin, pull-value resolver, missing key → `null`, unclosed `${` literal, overflow detected |
| template types | bool/i64/${source}.${name} formatting |

## Expected result

```
8 Tests 0 Failures 0 Ignored
OK
```
