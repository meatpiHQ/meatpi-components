# bridge_manager — on-target test app (self-contained, lwIP loopback)

Registers four endpoints (stubs `src`/`dst`/`echo` + the real
`socket_manager` server `tcp0` wrapped in the promised three-line glue),
persists the bridge set before the settings boot pass, and runs three legs.
Run:

```powershell
.\test.ps1 target bridge_manager
```

## What is covered

1. **Stub pump** (`br0`: src→dst raw): 2000 sequenced chunks pumped ordered
   and lossless; prints the measured **pump ceiling** (chunks/s + KB/s —
   BENCHMARKS.md scenario 1).
2. **Overflow**: dst goes slow (20 ms per send) while src floods 200 chunks
   with zero-wait sends — drops happen at the PRODUCER (bounded queue,
   drop-and-count), the pump survives, accounting stays consistent.
3. **Socket end-to-end** (`br1`: tcp0↔echo raw): a real TCP client on
   127.0.0.1:3333 sends a payload and receives it echoed through
   socket_manager → bridge pump → echo endpoint → pump → socket_manager —
   the full production path in one device.

Also: duplicate endpoint registration rejected, registry-aware settings
validation (`CFG-BAD`: unknown endpoint rejected by `on_validate`), clean
`stop()`.

## Expected result — serial markers, in this order

```
INIT ok=1
ENDPOINTS ok=1 dup_rejected=1
CFG ok=1 err=''
CFG-BAD rejected=1
START ok=1
PUMP ok=1 in_order=1 drops=0
PUMP-CEILING chunks_per_s=<n> kbytes_per_s=<n>
OVERFLOW drops_gt0=1 pump_alive=1 accounted=1
E2E ok=1 match=1
STATS br1 a2b=<n> b2a=<n> errors=0
STOP ok=1
TEST DONE
```

**Last verified green: 2026-07-03 on WiCAN Pro (first flight). Measured
pump ceiling: 38,260 chunks/s ≈ 4.7 MB/s** (recorded in `../BENCHMARKS.md`
§1). Inherits the main config via root `sdkconfig.defaults`; the overlay
pins the test console to 115200 (main runs 2 Mbaud).

The real-hardware pairing (OBD chip ↔ TCP over Wi-Fi against the live OBD
bench + rpi001) lives in `../test_apps_bench/` — **GREEN 2026-07-03**, see
its README + `../BENCHMARKS.md` §5.
