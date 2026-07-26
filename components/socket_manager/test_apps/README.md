# socket_manager — on-target test app (self-contained, lwIP loopback)

Persists a test server set (TCP `tcp0:3333` max 2 clients + UDP `udp0:17`)
**before** the settings boot pass (`set()` then `settings_manager_start()` —
reboot-to-apply without a reboot), then drives real BSD-socket clients
against 127.0.0.1. No RF, no external gear. Run:

```powershell
.\test.ps1 target socket_manager
```

## What is covered

Settings array config through the real pipeline, subscribe (+ duplicate
subscriber rejected), TCP RX into the chunk queue, TX to a client,
multi-client aggregate semantics (fan-out TX to two clients, merged RX),
max_clients accept-then-close + `refused` counter, abrupt-close reaping
(remaining client keeps working), UDP no-peer send refusal, UDP RX +
last-peer TX, stats counters.

## Expected result — serial markers, in this order

```
INIT ok=1
CFG ok=1 err=''
START ok=1
SUB ok=1 dup_rejected=1
TCP-RX ok=1 match=1
TCP-TX ok=1 match=1
FANOUT ok=1
MERGE ok=1
MAXCLIENTS closed=1 refused=1
REAP ok=1 clients=1
UDP-NOPEER refused=1
UDP-RX ok=1
UDP-LASTPEER ok=1
STATS in=<n> out=<n> rx_drops=0
TEST DONE
```

**Last verified green: 2026-07-03 on WiCAN Pro (first flight — all 13
markers).** Inherits the main config via root `sdkconfig.defaults`
(standard §7); the overlay pins the test console to 115200 (main runs
2 Mbaud — sanctioned test-console override).

Wi-Fi robustness (listener recovery across a Wi-Fi drop) and the RF
benchmark baselines are bench scenarios — see `../BENCHMARKS.md`.
