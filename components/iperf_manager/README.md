# iperf_manager — link-throughput measurement (feature)

Thin lifecycle + CLI wrapper around the managed **`espressif/iperf`**
engine (v1.0.2 — the esp-idf iperf example matured into a component).
On-demand diagnostic: nothing runs until the operator starts a session
from the console; sessions are time-bounded and never persist across
reboots (deliberately no autostart setting — an open traffic sink is a
diagnostic, not a service).

**Peer compatibility: iperf 2.x ONLY** (default port 5001). iperf3 is a
different protocol and will not connect. On Debian/the bench Pi:
`apt install iperf`. For plain TCP legs a trivial drain/push script is
enough — iperf2's TCP data path is a raw byte stream (the bench does
exactly that on the PC side, `tools` note below).

## CLI (`iperf`)

```
iperf -s [-u] [-p port] [-t secs] [-i secs]                    server
iperf -c <ip> [-u] [-p port] [-t secs] [-i secs] [-l len] [-b Mbits/s]
iperf -r        latest period/summary numbers (remote-CLI friendly)
iperf -a        abort
```

- Interval/summary lines print asynchronously on the **serial console**
  (the engine's report task). Remote CLI sessions (ws_cli/TCP/BLE) poll
  `iperf -r` — the weak `iperf_report_output` override caches the last
  period/summary under a mutex besides chaining to the default printer.
- One CLI-tracked session at a time (`-a` to clear); the engine itself
  supports several instances.
- IPv4 dotted-quad hosts only (no DNS — bench peers are IPs).

## Engine gotchas (bench, 2026-07-11)

- **The server's accept window is 5 s** (`IPERF_SOCKET_ACCEPT_TIMEOUT`),
  then the engine ABORTS the instance ("errno 11") — start the peer
  client promptly after `iperf -s`.
- An errored/aborted instance emits NO summary report — session
  liveness is tracked via the `state_handler` callback (`IPERF_CLOSED`
  fires on every termination path), not the report stream.
- `bw_lim` is bits/s inside the engine; the CLI's `-b` takes Mbit/s.

## Measured (2026-07-11, WiCAN Pro; WiFi = 2.4 GHz HT20 vs rpi001's AP)

| Leg | Result |
|---|---|
| DUT→PC TCP over **USB-NCM** (CDC-NCM, FS PHY) | TX **5.2** / RX **4.7 Mbit/s** (PC-side agrees) |
| DUT↔Pi TCP over **USB-Ethernet** (ASIX, wired to Pi eth0) | TX **6.5** / RX **7.2 Mbit/s** (2026-07-13; rock-steady intervals, both sides agree) |
| DUT→Pi UDP `-b 20` over USB-Ethernet | **6.8 Mbit/s** sent, **0% loss**, jitter 1.7 ms — the FS-USB saturation point |
| Re-measured 2026-08-26 (AX88772B → cdc_ncm adapter on Pi `eth1`, 10.42.2.x, espnetlink build v4.51p_beta) | TCP TX **6.45** / RX **7.71 Mbit/s**, UDP `-b 20` **7.00 Mbit/s** sent, **0% loss**, jitter 1.68 ms — no drift from the 07-13 baseline |
| DUT→Pi TCP over WiFi STA | **16.2 Mbit/s** avg (19 steady after ramp) |
| Pi→DUT TCP over WiFi STA | **12.6 Mbit/s** avg (both sides agree; one mid-run RF dip to 4) |
| DUT→Pi UDP `-b 20` over WiFi | **14.8 Mbit/s** sent; Pi reports loss/jitter per interval (the UDP protocol works against real iperf2) |
| DUT→internet via **ESPNetLink LTE** (BG95 Cat-M1, NCM host mode) | **0.11 Mbit/s** UL avg (2026-07-13; the Pi server behind a router port-forward saw the Telstra carrier IP, irtt 410 ms — required the dongle's `NCM_SHARE` on + the cherryusb host ZLP fix). Uncapped TCP self-throttles to link rate: 8 s ≈ 115 KB — IoT-SIM-safe; use `-t`/`-b` for tighter budgets |

The S3's USB is Full-Speed (12 Mbit/s line rate) — ~5-6 Mbit/s of TCP
goodput over CDC-NCM is the expected ceiling, not a bug. WiFi numbers
share air with the bench AP; expect run-to-run variance.

## Settings (`"iperf_manager"`, version 1, field table)

`cli` (bool, true) — register the `iperf` command on the boot apply.

## Memory

Per running session (engine-owned, freed at session end): traffic task
4 KB + report task 4 KB (internal-RAM stacks, `xTaskCreate`), TCP
buffer 16 KB (heap → PSRAM at our threshold). Idle: a mutex + ~40 B.

## Dependencies

`espressif/iperf` (managed, via this component's `idf_component.yml`),
`cmdline_manager`, `settings_manager`, `log_manager`.
