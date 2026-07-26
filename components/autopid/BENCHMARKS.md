# autopid — polling performance (Phase 1b)

Bench: WiCAN Pro DUT (id 14c19f44e349) + MIC3624 ELM327 v2.3 @ 2 Mbaud
UART + ECU simulator on 500 kbit/s CAN, protocol 6 (11-bit). Measured
2026-07-06 on the composed main firmware via `/api/autopid` stats
deltas, cross-checked passively on the wire (PCAN observer). Standard
mode-01 PIDs, single-frame responses, headers off.

## The ceiling: ~19 requests/s (53 ms/request)

`period_ms: 0` (high-fidelity max-rate mode), 20 s windows:

| PIDs at period 0 | total polls/s | per-request | per-PID rate |
|---|---|---|---|
| 1 | 18.8 | 53.2 ms | 18.8/s |
| 2 | 18.8 | 53.1 ms | 9.4/s |
| 4 | 18.8 | 53.1 ms | 4.7/s |
| 8 | 18.9 | 52.9 ms | 2.4/s |

The chip+ECU round trip is the whole story: total throughput is flat at
~19 req/s regardless of table size, so N max-rate PIDs each poll at
19/N per second. Wire check (PCAN, 8-PID table, 15 s): 19.0 req/s,
35–36 requests per PID (perfectly even), request-gap p50/p95/max =
53.0/53.9/54.0 ms, **zero** consecutive same-PID requests — the
scheduler's last-run tiebreak round-robins strictly and adds no jitter
of its own.

Failure counters stayed 0 throughout; 0 polls lost.

## Where the 53 ms goes — and the 2.5× lever

Decomposed with a PCAN fast ECU (sub-ms responder answering `0162`,
which the sim doesn't support) and the ELM response-count hint (a
trailing digit on the command, e.g. `010C1` = "expect ONE response,
return immediately"):

| leg | ECU | hint | rate | per-request |
|---|---|---|---|---|
| A | simulator | no | 18.9/s | 53.0 ms |
| B | simulator | **`010C1`** | **46.7/s** | **21.4 ms** |
| C | PCAN (sub-ms) | no | 18.8/s | 53.3 ms |
| D | PCAN (sub-ms) | `01621` | 47.6/s | 21.0 ms |

C = A: **the ECU's response latency is irrelevant** — a much faster ECU
changes nothing without the hint. ~32 ms of the 53 is the ELM327
lingering after the first response waiting for possible additional
ECUs (adaptive timing can only shrink, not remove, that window). The
remaining ~21 ms is chip-internal (the firmware request path blocks
event-driven on the UART queue; CAN + UART transfer is <1 ms).

**Today's user-accessible lever**: put the response-count digit in the
PID's `cmd` (`"010C1"`) → ~47 polls/s, 2.5× the unhinted ceiling. The
official WiCAN tester protocol uses the same trailing-digit hint. The
guard and parser handle the digit (pair alignment). Caveat: the digit
caps how many ECUs the ELM listens for — correct for the typical
single-responder PID, wrong for multi-ECU broadcast scans (`0100`
support bitmaps from every ECU). A per-PID `responses` field / auto-
hint default is a design decision for Phase 2+.

Practical guidance:
- One "fast" PID can genuinely run at ~19 Hz unhinted, ~47 Hz with the
  count hint. A dashboard's worth (8 PIDs) at max rate = ~2.4 Hz each
  (~5.9 Hz hinted); if you need 10 Hz on one signal, put ONLY that PID
  (hinted) in a period-0 group and schedule the rest at 500–1000 ms.
- Multi-frame responses (VIN, DTC lists) stretch the floor;
  vehicle-specific (29-bit / KWP) protocols will differ — re-measure
  per vehicle if it matters.

## The floor flag

`AP_PERIOD_FLOOR_MS = 50` (autopid_private.h). A configured period of
1–49 ms cannot be honored (the chip answers ~every 53 ms) — it is
accepted (period 0 remains the sanctioned max-rate mode) but flagged:
`GET /api/autopid` → `stats.period_floor_ms` + `stats.sub_floor_pids`
(count of enabled PIDs whose configured/inherited period is sub-floor).
UIs should warn on `sub_floor_pids > 0`.

## BLE advertising coex: negligible

8-PID max-rate table, BLE advertising (bench default) vs BLE disabled:

| BLE | polls/s | per-request |
|---|---|---|
| advertising | 18.9 | 52.9 ms |
| off | 19.1 | 52.2 ms |

~1% — polling rides the chip UART, not the radio. (BLE/WiFi coex still
costs the NETWORK transports 25–55%, see the 2026-07-05 bench
re-baseline — deliver autopid data over MQTT/WS and that path pays it.)

## Sharing the chip (WS-OBD bridge + autopid)

The v1 arbitration policy is documented in obd_chip.h: bridge raw
traffic (`obd_chip_send`) does NOT take the COMMAND claim, so a WS-OBD
client co-masters the chip with autopid and responses interleave into
each other's windows.

Measured (autopid 010C at period 0, WS client injecting `0105` at
10/s — 99 injections reached the chip over 20 s):

- autopid held 19.4/s ok, 0.15/s failed — the garbled windows were
  **rejected**, not mis-parsed;
- cached values stayed correct throughout (rpm never deviated) thanks
  to the cross-talk guard: `ap_payload_matches_cmd()` requires the
  payload to echo `(service | 0x40)` + the identifier byte before
  anything is cached (autopid.c run_pid; host-tested).

An earlier unguarded run (WS client at max rate) saw ~10 failed/s under
heavy contention — self-throttling via the ×4 fail backoff. Guidance:
occasional WS-OBD console use alongside autopid is safe (wrong-window
data cannot enter the cache); for clean high-fidelity capture, don't
run a second chip master. ATMA filters (Phase 4) take the MONITOR claim
and pause polling properly.

## Event-pipeline cost — DEFERRED to Phase 3

`min_event_interval_ms=0` firehose characterization needs event_manager
(autopid.param events don't exist yet). The 10 ms schema floor ships on
the polling numbers alone: one sample per poll = max ~19 events/s per
PID, well inside any 10 ms interval — re-measure once the dispatcher
exists to quantify rule-evaluation cost at that rate.
