# ble_manager — benchmarks

> Measured 2026-07-04 on the live bench. DUT: WiCAN Pro (ESP32-S3),
> ble_manager test app (ble↔echo raw bridge + `blast` CLI command),
> IDF v6.0.2, data-path working tree. Client: **rpi001** (Pi 5 onboard BLE,
> BlueZ 5.66, bleak 3.0.2), ~2 m, TX power +9 dBm, legacy conn params
> (20–40 ms interval), pairing SC+MITM static passkey. Load generator:
> `tools/testbench/ble_perf.py` (run ON the Pi) — every number is
> reproducible from that script + this app.

## 1. TX (notify) throughput — device → client

The DUT pushes 256 KB through `ble_manager_send()` (paced against the
bounded TX queue like a real bridge producer); the client times
first→last byte on FFF1 and verifies the count.

| Bytes | Time | Throughput | Loss | Date |
|---|---|---|---|---|
| 262,144 / 262,144 | 4.3 s | **60.2 KB/s** | 0 | 2026-07-04 |

This is the ceiling for OBD/CAN→BLE streaming — comfortably above a busy
ELM327 dialog and roughly a half-loaded 500 kbit/s CAN bus of payload.

## 2. Echo (bidirectional) throughput

Client writes FFF2 (write-without-response, 244 B) flat out for 20 s while
draining the echoed FFF1 stream — every byte crosses the bridge pump twice.

| Each way | Aggregate through the pump | Date |
|---|---|---|
| **37.9 KB/s** | ~76 KB/s | 2026-07-04 |

## 3. Round-trip latency (write → pump → echo → pump → notify)

100 samples:

| p50 | p95 | p99 | Date |
|---|---|---|---|
| **4.6 ms** | 12.7 ms | 35.6 ms | 2026-07-04 |

(An earlier session measured p50 ≈ 75 ms right after connect — before the
DUT's requested 20 ms connection interval took effect. Steady-state is the
number above; expect the first seconds after connect to be slower.)

## 4. Connection-profile A/B (`conn_profile` setting, added 2026-07-04)

The interval the device REQUESTS on connect is a setting: `ios` (default,
legacy 20–40 ms) vs `android_fast` (7.5–15 ms). Measured back-to-back on
the Pi bench (same script, fresh bond each run):

| Profile | TX notify | Echo each way | RTT p50/p95 | Date |
|---|---|---|---|---|
| `ios` (default) | 60.2 KB/s | 37.9 KB/s | 4.6 / 12.7 ms | 2026-07-04 |
| `android_fast` | 62.8 KB/s | 24.4 KB/s | 4.7 / 22.9 ms | 2026-07-04 |

**Interpretation:** within run-to-run noise — a steady-state RTT of ~4.6 ms
is impossible at a 20–40 ms interval, so **BlueZ (the central) was already
running a short interval regardless of the request**; this bench cannot
show the profile's effect. The setting matters against MOBILE centrals:
iOS enforces ≥15 ms (the default profile keeps Apple's rules), Android
honors 7.5 ms when asked (`android_fast` = the max-performance choice).
The echo delta is client-side (bleak/D-Bus per-write latency), not the
profile. Decision 2026-07-04 (meatpi): stay on **1M PHY / BLE 4.2** — the
numbers above are the realistic envelope for that configuration; remaining
headroom is TX-path software tuning (queue depth, credit-event wakeup)
and native-client measurement.

## Notes

- **MTU confirmed on-wire (2026-07-04)**: the DUT logs
  `MTU 517 (payload 490)` when the Pi connects — the full exchange happens;
  bleak's `mtu: 23` is purely a BlueZ-backend reporting artifact. So there
  is NO hidden MTU headroom: notifies already carry 490 B payloads, and the
  ~60–70 KB/s TX ceiling is radio-credit/interval bound (~130–145
  notifies/s), exactly where 1M-PHY Bluedroid is expected to sit.
- The same capture shows the TX path's bounded-queue backpressure working
  under blast load: `tx queue full` warnings while the producer out-runs the
  radio, zero bytes lost (the producer paces and retries — the endpoint
  contract). The 32×128 B TX queue is the throttle point; deepening it +
  credit-event wakeup is the remaining software headroom.
- Re-run when conn parameters, TX queue depth (32×128 B), send-buffer size
  (490 B) or the BT stack config change. Remove a stale bond on the Pi
  first (`bluetoothctl remove <mac>`) — a re-flashed DUT with an old bond
  on the client side disconnects during service discovery.
