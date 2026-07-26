# bridge_manager — benchmarks (spec §8, required deliverable)

> Status 2026-07-03 (evening bench session): scenario 1 measured on
> hardware; the §5 equivalent (OBD ↔ TCP over Wi-Fi) ran GREEN on the live
> bench — see below. Remaining: translator scenarios (blocked on v2
> translators) and the multi-bridge concurrency sweep.

Fixed configuration to record with every run: chunk 128 B, queue depth 32,
pump prio 10, PSRAM stacks, IDF v6.0.2, firmware commit hash.

## 1. Pump ceiling (raw bridge, stub loopback endpoints — no real I/O)

Printed by the test app (`PUMP-CEILING chunks_per_s= kbytes_per_s=`).

| chunks/s | KB/s | Commit | Date |
|---|---|---|---|
| **38,260** | **4,782 (~4.7 MB/s)** | working tree (data-path track), IDF v6.0.2 | 2026-07-03 |

Context: a 100 %-loaded 500 kbit/s CAN bus is ~3–4 k frames/s — the pump has
~10× headroom over the spec's headline scenario before any real I/O cost.

## 2. Translator overhead (raw vs slcan vs gvret, same load)

Translators landed 2026-07-08 (slcan, realdash, gvret) + the `can` endpoint.
Codec overhead is negligible vs. the CAN bus + transport: a frame is ≤14 B on
the internal wire and encode/decode are branchless byte loops (no alloc, no
I/O). The binding limit is the transport, measured in §5 below.

| Path (USB-NCM host) | RX CAN→client fps | TX client→CAN fps | Limit | Date |
|---|---|---|---|---|
| slcan over TCP `slcan0:3333` | **4405** (line rate) | **4606** (line rate) | 500 kbit/s bus | 2026-07-08 |
| slcan over WS `/ws/can` | **676** | **4445** (line rate) | httpd WS frame rate | 2026-07-08 |

Both directions of both transports also pass the functional benches
(`slcan_bridge_test.py`, `slcan_ws_test.py` — SLCAN BRIDGE/WS PASS).

The WS RX ceiling (~676 fps ≈ the 2026-07-04 ws_bench ~700 fps figure) is one
CAN frame = one bridge chunk = one WS frame; httpd send-rate binds, and the
excess drops cleanly at the can-endpoint queue (verified: `/api/can` rx
counted every bus frame, `/api/ws` tx_drops 0, drop happens at the bounded
32-deep endpoint queue — exact accounting, nothing wedges). Chunk coalescing
in the can-endpoint pump is the known future lever if WS needs bus rate.

## 2b. Per-transport method (USB · WiFi · WS)

`tools/testbench/slcan_perf_test.py` floods each direction and reports
delivered frames/s:
- **USB-NCM TCP**: `--host 192.168.82.1 --port 3333` (PC-local PCAN).
- **WS**: `--ws [--ws-path /ws/can]` — needs bridge `can↔ws_can/slcan`
  (`slcan_ws_test.py` configures it). WS client = `wsmin.py`, a
  dependency-free RFC6455 client (pip was unavailable on the bench PC).
- **TCP / WiFi-STA** — MEASURED 2026-07-18: PCAN PC-side, socket reached
  via an ssh port-forward through rpi001 (`ssh -L 13333:<dut-sta-ip>:3333`,
  the PC cannot see the hotspot subnet directly; the forward hop is LAN +
  Pi CPU, not the bottleneck): **RX CAN→slcan 4338 fps (5.7 % drop of a
  paced 4600 flood), TX slcan→CAN 5350 fps** — both at/above the 500 kbit/s
  8-byte bus ceiling (~4200), same class as USB-NCM TCP. DUT STA on the
  rpi001 hotspot, ECU sim also live on the bus during the run.

**CHUNK COALESCING (2026-07-18):** the `can` endpoint pump now packs
every already-queued frame into one bridge chunk (a chunk = plain
concatenation of wire frames; all three translator codecs loop via
`can_wire_decode_next`, host-tested), and the slcan codec batches its
lines into ONE sink call per chunk. Frame-bound transports ride both:
WS RX measured **351 → 2,466 fps (7×)** on the identical path
(via-Pi ssh-forward; the historical direct-path number was 676).
Remaining drop at full 4,600-fps flood is the 32-deep endpoint queue
during httpd send stalls — deepen it if WS ever needs true bus rate.
Zero added latency on an idle bus (only already-waiting frames pack).
Clients must accept multiple CR-terminated slcan records per WS frame
(stream-legal slcan).

**PEAK-adapter bench traps** (both directions read 0–3 fps until understood):
PCAN `Write()` has NO backpressure — an unpaced flood leaves tens of thousands
of frames queued in the driver, the adapter goes error-passive mid-run (stops
ACKing), and a non-ACKing peer then walks the DUT's TWAI to error-passive too
(TEC pins at 128 — an error-passive transmitter's ACK errors don't increment
TEC, so it retries forever, and the stale PEAK queue blasts old frames into
the next session). The script now paces the flood (`--rx-fps`, default 4600 ≈
just over the ceiling) and re-inits the PCAN + heal-kicks the bus (one DUT-TX
frame both nodes re-integrate on) between the RX and TX legs. Hard-wedge
recovery: reboot the DUT (clears its TWAI queue) + PCANBasic
`Uninitialize(PCAN_NONEBUS)`; worst case replug the PEAK.

Firmware note from the same investigation: `can_manager` has no bus-off
recovery path (a true bus-off — TEC 256, e.g. shorted bus — would need
`twai_initiate_recovery` and currently stays down until reboot; error-passive
self-recovers once a peer ACKs again). Follow-up candidate, not a translator
issue.

## 3. Concurrency (1 / 2 / 4 bridges under load)

Extend the test app with N src→dst bridges; assert per-bridge fairness.

| Bridges | Aggregate chunks/s | Min per-bridge | Commit | Date |
|---|---|---|---|---|
| 1 | 37,975 (pump ceiling, leg 1) | — | 9f0a04a+wt | 2026-07-18 |
| 2 | 37,079 | 18,539 (perfect 50/50, fair=1) | 9f0a04a+wt | 2026-07-18 |

2026-07-18 note: the settings schema caps `bridges` at **maxItems 4**, and
the app reserves one slot for the tcp0↔echo E2E bridge — a 4-loaded-bridge
row isn't reachable without raising the cap. At 2 loaded bridges the pump
splits its ceiling exactly evenly with no aggregate loss. Also learned the
hard way: pre-start `settings_manager_set` is deliberately LENIENT
(dynamic jacks register between boot pass and start), so the app's
unknown-endpoint negative probe must run AFTER `bridge_manager_start()` —
run earlier it is accepted and clobbers the good config (the app
crash-looped on a NULL src queue until reordered).

## 4. Overload behavior

Covered qualitatively by the test app's OVERFLOW leg (drop-and-count at the
producer, pump alive, exact accounting). Record heap high-water before/after
a 60 s overload here:

| Heap HW before/after | Drops counted | Surviving-chunk latency bound | Commit | Date |
|---|---|---|---|---|
| 333,111 / 333,111 (leak 0 B) | 20,477,075 of 20,480,108 offered (exact: drops+delivered==sent) | queue drained ≤1.5 s after the consumer unblocked; pump alive throughout | 9f0a04a+wt | 2026-07-18 |

## 5. Full-bus CAN case (headline number — bench, PCAN)

500 kbit/s bus at ~100 % load (~3–4 k frames/s) through CAN→gvret→TCP.
**Blocked on the internal-CAN endpoint component + translator_gvret** (not
in v1 scope). The equivalent available today: OBD ↔ TCP raw bridge on the
live OBD bench — DUT joins rpi001's AP, `tcp0:35000` bridged to the `obd`
endpoint, a TCP client sends `ATI\r`/`0100\r` and monitor-mode `ATMA`
streams at bus rate. Procedure: register obd endpoint glue in a
wifi+socket+bridge+obd composition, run `tools/testbench/obd_bench_check.py`
pointing at `<dut-ip>:35000` instead of COM2029.

**RESULT 2026-07-03 (available-today equivalent, `test_apps_bench`
composition, rpi001 AP at −47 dBm):** the OBD ↔ TCP raw bridge over Wi-Fi is
fully functional against the live bench — from rpi001, `ATI` → `ELM327
v2.3`, `VTVERS` → `MIC3624 V2.3.22`, `0100` → live ECU-sim reply, and the
multi-frame VIN (`0902`, 3 ISO-TP lines) all round-trip through
socket_manager → bridge pump → obd_chip → CAN bus and back. Echo-bridge RF
numbers (2× pump per byte): 224 KB/s each direction simultaneously; RTT
p50 3.4 ms / p95 14.2 ms / p99 21.3 ms; Wi-Fi drop → 15 s to full recovery.
The CAN→gvret→TCP headline row stays open for the translator milestone:

| Scenario | frames/s sustained | Drop rate | Commit | Date |
|---|---|---|---|---|
