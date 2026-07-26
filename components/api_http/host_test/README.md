# api_http — host unit suite

Compiles ONLY the pure utility layer (`api_http_util.c` — no
esp_http_server dependency) on the IDF **linux** target. Runs on the bench
Pi via `.\test.ps1 host` (or manually: `idf.py --preview set-target linux &&
idf.py build && ./build/api_http_host_test.elf`).

## What is covered (7 tests)

- Password redaction: every `*_password` string key → `""`; non-suffix keys
  (`password_hint`) and non-password fields untouched.
- Unredaction (PUT semantics): `""` password restored from the stored
  object; a real new value passes through; no stored value → `""` stands.
- Settings route parsing: `/api/settings/<name>` and `<name>/schema`;
  rejects empty name, extra segments, bare `/schema`, wrong prefix, and
  name-buffer overflow.
- Log level mapping: all six names → `esp_log_level_t`; unknown/NULL
  rejected.

## Expected result

```
7 Tests 0 Failures 0 Ignored
OK
```

(The FreeRTOS POSIX ELF never exits by itself — the runner judges by the
Unity summary line, same as the other host suites.)
