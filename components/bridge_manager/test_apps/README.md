# bridge_manager: on-target test app (self-contained, lwIP loopback)

Registers four endpoints (stubs `src`/`dst`/`echo` + the real
`socket_manager` server `tcp0` wrapped in the promised three-line glue),
persists the bridge set before the settings boot pass, and runs three legs.
Run:

```powershell
.\test.ps1 target bridge_manager
```

## What is covered

1. **Stub pump** (`br0`: src→dst raw): 2000 sequenced chunks pumped ordered
   and lossless; prints the measured **pump ceiling** (chunks/s + KB/s:
   BENCHMARKS.md scenario 1).
2. **Overflow**: dst goes slow (20 ms per send) while src floods 200 chunks
   with zero-wait sends, drops happen at the PRODUCER (bounded queue,
   drop-and-count), the pump survives, accounting stays consistent.
3. **Socket end-to-end** (`br1`: tcp0↔echo raw): a real TCP client on
   127.0.0.1:3333 sends a payload and receives it echoed through
   socket_manager → bridge pump → echo endpoint → pump → socket_manager:
   the full production path in one device.

Also: duplicate endpoint registration rejected; the runtime PUT rules after
start (`CFG-BAD`: an unknown translator refused by `on_validate`; `CFG-NEW`:
an unknown endpoint accepted since 2026-10-29, the live bridges untouched);
a row refused at start by a single-stream jack already in use (`REASON`:
`o2` on the stub `one` beside `o1`) and six stop/start cycles that prove the
refusal leaves no queue set behind (`CYCLES`, the internal heap before and
after; 2026-10-29); clean `stop()`.

## Expected result: serial markers, in this order

```
INIT ok=1
ENDPOINTS ok=1 dup_rejected=1
CFG ok=1 err=''
START ok=1
CFG-BAD rejected=1
CFG-NEW accepted=1 live=1
PUMP ok=1 in_order=1 drops=0
PUMP-CEILING chunks_per_s=<n> kbytes_per_s=<n>
OVERFLOW drops_gt0=1 pump_alive=1 accounted=1
E2E ok=1 match=1
STATS br1 a2b=<n> b2a=<n> errors=0
CONC bridges=2 agg_chunks_per_s=<n> min_chunks_per_s=<n> fair=1
OVERLOAD60 heap_before=<n> heap_after=<n> leak=<n> ... pump_alive=1
REASON o2='an endpoint is already used by another bridge' o1_up=1 o2_up=0
CYCLES n=6 ok=1 leak=<n> leak_ok=1
STOP ok=1
TEST DONE
```

**Last verified green: 2026-10-29 on the bench DUT (68ee8f5a653d) through
`.\test.ps1 target bridge_manager`: all 16 markers in order, 0 E lines, the
8 W lines expected (the refused `o2` at the first start and at each of the
six cycles, the deliberate `CFG-BAD`), `CYCLES leak=-128`, `OVERLOAD60
leak=0`. Measured pump ceiling: 104,515 chunks/s ≈ 13 MB/s** with the raw
coalescing (the July first flight measured 38,260 chunks/s ≈ 4.7 MB/s per
chunk, recorded in `../BENCHMARKS.md` §1). Between July and 2026-10-29 the
app could not link (every on-target app in the tree: `ip4_routes`' wrap
without a `-u`, see its README), and two legs had gone stale unnoticed: the
`dst` stub counted sends and read the first byte only, which the coalescing
turned into `PUMP ok=0 in_order=0` (it walks the batch chunk by chunk now),
and the CONC leg's busy loop starved IDLE0 into task-watchdog E lines (it
yields every 1024 rounds now). Inherits the main config via root
`sdkconfig.defaults`; the overlay pins the test console to 115200 (main
runs 2 Mbaud). `serial_capture.py` judges only `TEST DONE` and crash
signatures: read the markers against `pytest_bridge_manager.py` (or run the
pytest) to judge a run.

The real-hardware pairing (OBD chip ↔ TCP over Wi-Fi against the live OBD
bench + rpi001) lives in `../test_apps_bench/`: **GREEN 2026-07-03**, see
its README + `../BENCHMARKS.md` §5.
