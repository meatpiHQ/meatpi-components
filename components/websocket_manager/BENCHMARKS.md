# websocket_manager — benchmarks

> Status 2026-07-04: measured on the **composed main firmware** (all 15
> components live — WiFi STA + BLE advertising + OBD + SD + httpd), not a
> stripped bench app: these are conservative product numbers. Driven from
> rpi001 by `tools/testbench/ws_bench.py` over a CROSS-TRANSPORT bridge
> `br_ws = ws_obd <-raw-> obd0` (WS channel ↔ TCP server; the Pi holds both
> ends), so every byte crosses httpd WS framing + the bridge pump + lwIP
> TCP. One pump crossing per direction — socket_manager's echo-bridge rows
> crossed the pump twice; the `latency` scenario here loops WS→TCP→WS so it
> is directly comparable to those RTT rows. RF context: rpi001 hotspot
> (Pi 5 + RTL8822CU), ~2 m. IDF v6.0.2.

## Two DUT-side fixes found BY this bench (both in websocket_manager_ws.c)

1. **~45 ms/frame WS TX stall**: httpd sends a WS frame as TWO `send()`
   calls (header, then payload); with Nagle the payload waits on the peer's
   delayed ACK of the 2-byte header. Measured leg-split: WS→TCP 2.4 ms,
   TCP→WS 47.3 ms. Fix: `TCP_NODELAY` on the client fd at handshake →
   round-trip p50 51.8 → 7.1 ms.
2. **RX starvation under TX load**: `wsm_ws_send` held `s_lock` across the
   network sends, so at ~700 frames/s TX the RX handler starved on the
   mutex (bidirectional WS→TCP collapsed to 2 KB/s). Fix: snapshot the fd
   list, send outside the lock, re-take for reap/stats. Also: stale-client
   slots are now reaped at handshake time too (a quiet channel used to
   fill with ghosts and refuse new clients until the next TX).

## How to run

Configure the cross-transport bridge + enable `ws_obd`, submit-reboot, then
from rpi001:

```
python3 tools/testbench/ws_bench.py --host <dut-ip> --scenario \
    ws_to_tcp|tcp_to_ws|bidir|latency|fanout|obd_poll [--secs 20] [--frame 128]
```

`obd_poll` needs `br_obd = obd <-> ws_obd` instead (and the live OBD bench).

## 1. Throughput (single client, via the ws_obd↔obd0 bridge)

| Direction | Frame | Offered KB/s | Delivered KB/s | Loss % | Commit | Date | RF |
|---|---|---|---|---|---|---|---|
| WS → TCP | 128 B | 87.1 | 84.8 | 2.5 | v6-dev tree | 2026-07-04 | −47 dBm |
| TCP → WS | 128 B | 86.7 | 84.9 | 2.0 | v6-dev tree | 2026-07-04 | −47 dBm |
| WS → TCP | 1024 B | 359.3 | 359.3 | 0.0 | v6-dev tree | 2026-07-04 | −47 dBm |
| TCP → WS | 1024 B | 83.9 | 83.3 | 0.7 | v6-dev tree | 2026-07-04 | −47 dBm |

The ~85 KB/s rows are FRAME-RATE-bound (~680 WS frames/s), not byte-bound:
every 128 B bridge chunk becomes its own WS frame (two sends in httpd), so
TCP→WS doesn't improve with bigger TCP writes. Larger frames only help the
WS→TCP direction (fewer, bigger RX frames → 4.2×); DUT-side TX coalescing
(N chunks → one frame) is the lever if more is ever needed.

## 2. Round-trip latency (8 B, WS→TCP→WS loop = pump ×2, radio ×2)

| Path | n | p50 ms | p95 ms | p99 ms | Commit | Date |
|---|---|---|---|---|---|---|
| Wi-Fi | 749 | 5.4 | 14.7 | 25.9 | v6-dev tree | 2026-07-04 |

socket_manager's TCP echo-bridge row (same double-crossing shape): p50 3.4 /
p95 14.2 — the WS delta is httpd framing + one extra client hop on the Pi.

## 3. Bidirectional (simultaneous blast both directions, 128 B)

| WS→TCP KB/s | TCP→WS KB/s | Aggregate KB/s | Commit | Date |
|---|---|---|---|---|
| 9.0 | 73.7 | 82.7 | v6-dev tree | 2026-07-04 |

The aggregate lands on the SAME ~83–87 KB/s ceiling as a single direction:
the ~700 frames/s budget is shared between RX (httpd task) and TX (bridge
pump), and the split under full saturation is scheduling-dependent (a
repeat run split 60.6/0.0 the other way; no clients were dropped — log
ring verified). Under real workloads (one direction saturating at most)
this doesn't bite; provision per-direction, don't assume fair sharing at
saturation.

## 4. Fan-out (TCP blasts, 2 WS clients must both get the full stream)

| Clients | Offered KB/s | Per-client KB/s | Aggregate out KB/s | Commit | Date |
|---|---|---|---|---|---|
| 2 | 83.4 | 45.2 / 45.2 | 90.4 | v6-dev tree | 2026-07-04 |

Fan-out TX shares the same frame-rate budget: 2 clients ≈ half rate each
(aggregate out ~90 KB/s ≈ the TX ceiling), split evenly.

## 5. OBD polling over WS (br_obd = obd↔ws_obd, live MIC3624 + ECU sim)

| Path | Polls | p50 ms | p95 ms | Commit | Date |
|---|---|---|---|---|---|
| Wi-Fi, 0100 poll loop | 520 / 30 s | 54.5 | 69.0 | v6-dev tree | 2026-07-04 |

system_bench's TCP polling row for comparison: 410 polls/30 s, p50 68 ms —
WS is actually FASTER here (TCP_NODELAY on the WS side; the ELM-chip
turnaround dominates both).

Re-run when httpd config, chunk size, task priorities, or lwIP config
change.
