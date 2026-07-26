# socket_manager — host unit suite

Compiles ONLY the pure policy layer (`socket_manager_policy.c` — no lwIP)
on the IDF **linux** target. Runs on the bench Pi via `.\test.ps1 host`.

## What is covered (9 tests)

- Backoff progression 1→2→4→8 s with cap (listener-recovery pacing).
- Accept/reject decision at max_clients (accept-then-close policy input).
- Config item parse: code defaults (max_clients 2, keepalive 30 s,
  disabled), name charset rejection.
- Cross-item validation: valid sets accepted; duplicate names rejected;
  enabled-port clashes rejected; a DISABLED server may park an enabled
  server's port; max_clients bounds.

## Expected result

```
9 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-07-03 on rpi001.
