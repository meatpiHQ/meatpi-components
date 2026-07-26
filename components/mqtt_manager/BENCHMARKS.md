# mqtt_manager — benchmarks

> Status 2026-07-04: measured on the **composed main firmware** (all
> radios + peripherals live), device on the bench AP (rpi001 hotspot,
> ~2 m), broker = mosquitto on rpi001, QoS 0. Driven by
> `tools/testbench/mqtt_bench.py` against the device's MQTT bench surface
> (`main/main_bench.c`: `<prefix>/bench/cmd` burst + `/bench/echo`
> bounce) — which also exercises the handler registry end to end.
> All rows use `publish_async` (the hot path).

## 1. Drain ceiling (unpaced burst — the ring fills in ~40 ms, then the
   publisher task drains at link speed; drops are the DESIGN working:
   producer-side, counted, non-blocking)

| Payload | Delivered msg/s | Delivered KB/s | Device enqueue side |
|---|---|---|---|
| 128 B | 1146 | 143 | ~50 k offers/s; ring absorbed 231, dropped the rest in 36 ms |
| 512 B | 801 | 400 | |
| 1 KB | 586 | 586 | |
| 4 KB | 316 | **1234** | |

The per-PUBLISH cost dominates: 32× bigger payloads cost only ~3.6× in
message rate → **8.6× more bytes/s**. This is the measured proof of the
"batch upstream" contract (legacy JSON-array pattern).

## 2. Sustained paced (zero-loss operating points)

| Payload | Rate | Duration | Drops |
|---|---|---|---|
| 1 KB | 100 msg/s (100 KB/s) | 30 s | **0** |
| 1 KB | 198 msg/s (198 KB/s) | 10 s | **0** |
| 512 B | 482 msg/s (241 KB/s) | 6 s | **0** |

(Rates limited by the test's pacing tick, not the link — the burst rows
above are the true ceiling.)

## 3. Round trip (Pi → `bench/echo` → handler → `publish_async` →
   `bench/echo_re` → Pi; the full RX-dispatch + async-TX machinery)

| Payload | n | p50 | p95 | p99 |
|---|---|---|---|---|
| 64 B | 200 | 5.0 ms | 9.6 ms | 20.6 ms |

## CAN→MQTT headroom (the question this bench answers)

A fully saturated 500 kbit/s bus ≈ 3.5 k frames/s ≈ 315 KB/s in the
legacy JSON-array encoding (~90 B/frame, batched ~11 frames per 1 KB
message ≈ 320 msg/s) — **under the measured 586 msg/s / 586 KB/s drain
ceiling at 1 KB**, with the 32 KB ring (~30 messages) riding out broker
hiccups. Bigger batches (2–4 KB) more than double the byte headroom.
Real busloads are far below saturation; PID-filtered publishing (the
legacy `canflt` mode) is negligible traffic.

Re-run when the WiFi link, broker, buffer sizes, or QoS defaults change.
