# socket_manager — benchmarks (spec §7, required deliverable)

> Status 2026-07-03 (evening bench session): **first Wi-Fi numbers measured**
> via the `bridge_manager/test_apps_bench` composition (wifi + socket +
> bridge + obd on the real bench; driven from rpi001 by
> `tools/testbench/socket_bench.py` against the echo bridge on :3334 — note
> every byte crosses the bridge pump twice, so these are conservative
> full-chain numbers, not raw lwIP). RF context: rpi001 hotspot
> (Pi 5 + RTL8822CU), RSSI −47 dBm, ~2 m. Firmware: data-path working tree,
> IDF v6.0.2. Loopback baselines + scenarios 2/5/6-full still to run.

Two baselines per scenario separate stack cost from radio cost:
**loopback** (127.0.0.1 in the test app, no RF) and **Wi-Fi** (DUT joined
rpi001's bench AP `WICAN_TEST_AP`; record RSSI, channel, AP = Pi 5 +
RTL8822CU hotspot).

## How to run

1. Loopback numbers: extend `test_apps` with the load loop (markers print
   `LOOPBACK-TCP kbytes_per_s=`), or take scenario 1 from the committed app.
2. Wi-Fi numbers: flash a wifi_manager + socket_manager composition
   (the Phase-8 main app once composed — until then reuse the HIL app plan),
   join the bench AP, then from rpi001 or the PC run
   `tools/testbench/socket_bench.py` (committed):

```
python tools/testbench/socket_bench.py --host <dut-ip> --port 35000 \
    --scenario tcp_throughput|udp_loss|latency|multiclient|soak
```

## Scenario tables (fill per run: config, result, commit, IDF, date, RF)

### 1. TCP throughput (single client, up / down / bidirectional)

| Path | Payload | Up KB/s | Down KB/s | Bidir KB/s | Commit | Date | RF |
|---|---|---|---|---|---|---|---|
| loopback | 128 B chunks | — | — | — | | | n/a |
| Wi-Fi (echo bridge = 2× pump) | 128 B chunks | 223.8 | 223.8 (echo) | ~448 aggregate | data-path tree | 2026-07-03 | −47 dBm |

### 2. UDP throughput + loss at increasing offered load

| Path | Offered KB/s | Delivered KB/s | Loss % | Commit | Date | RF |
|---|---|---|---|---|---|---|

### 3. TCP round-trip latency (small payload)

| Path | p50 ms | p95 ms | p99 ms | Commit | Date | RF |
|---|---|---|---|---|---|---|
| Wi-Fi (echo bridge, n=772) | 3.4 | 14.2 | 21.3 | data-path tree | 2026-07-03 | −47 dBm |

### 4. Multi-client fan-out cost (1 / 2 / 4 clients, one server)

| Path | Clients | Per-client KB/s | Aggregate KB/s | Commit | Date |
|---|---|---|---|---|---|
| Wi-Fi (echo bridge) | 4 | 7.8 / 13.6 / 14.0 / 11.2 | 46.7 in (≈187 out: every input echoes to ALL 4 clients — fan-out TX is O(clients), measured) | data-path tree | 2026-07-03 |

### 5. Concurrent servers (OBD-TCP + GVRET-TCP + UDP active)

| Path | Aggregate KB/s | Interference notes | Commit | Date |
|---|---|---|---|---|

### 6. Soak + churn (≥ 30 min, periodic client churn + one Wi-Fi drop)

| Heap high-water start/end | Listener recovery time | Counters consistent | Commit | Date |
|---|---|---|---|---|
| (soak pending) | **15 s** from AP resume to a working end-to-end echo (wifi reconnect + DHCP; ANY-bound listener needs no rebind) | ✓ | data-path tree | 2026-07-03 |

Re-run when buffer sizes, task priorities, or lwIP config change.
