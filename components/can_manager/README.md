# can_manager

Owner of the native CAN (TWAI) bus — WiCAN Pro TX=GPIO2 / RX=GPIO1 /
STDBY=GPIO38 (Kconfig `WICAN_CAN_*`). Wraps ONE shared `can_core`
handle (adopted code, see its PROVENANCE.md); every consumer
registers into it: add-on AT engines, autopid's `elm327` backend,
slcan/gvret/internal-CAN endpoints. Design +
status: `TASK_can_manager.md`.

## Settings (`can_manager`, v1, reboot-to-apply)

| key | type | default | |
|---|---|---|---|
| `enabled` | bool | false | opt-in |
| `baud` | enum | "500" | 33/83/95/100/125/250/500/1000 kbit/s |
| `silent` | bool | false | bus-wide listen-only (no ACK) |
| `cli` | bool | true | the `can` console command |

## Surfaces

- `GET /api/can` — status + stats incl. `state`, the bus-off/recovery
  counters, and the frame-loss localizers `rx_missed` (TWAI RX queue
  overflow = the can_core RX task starved) + `dispatch_drops`
  (drop-oldest in a subscriber queue = that consumer's drain starved)
  (HTTP_API §6e10).
- `can` CLI (`can -z` zeroes counters; `can send <id> [hexbytes] [-r]`
  transmits one frame — 3 hex digits = 11-bit id, more = 29-bit).
- C API: `can_manager_core_handle()` (the shared handle),
  `can_manager_send()`, `can_manager_subscribe_queue()` (drop-oldest
  frame fan-out), `can_manager_status()`.

## Bus-off recovery (2026-07-10)

A transmit-error cascade (shorted bus, baud-mismatch collisions) drives
TEC to 256 and the TWAI controller goes BUS-OFF — before this it stayed
down until reboot. Now `can_core`'s RX task services TWAI alerts:
`twai_initiate_recovery()` immediately on BUS_OFF (hardware waits for
128×11 recessive bits, i.e. completes only once the bus is sane), then
an automatic `twai_start()` after a policy backoff — immediate for an
isolated fault, 1→2→4…30 s cap when re-offending within 60 s (pure
policy in `can_core_recovery.c`, host-tested). Episodes are counted
in `/api/can` (`bus_off`/`recoveries`) and `can`; the web UI Status
tile flags `BUS-OFF · auto-recovering`. Fault-injection bench:
`tools/testbench/can_recovery_bench.py` (PCAN at a mismatched baud
blasting while the DUT transmits → real bus-off → verifies both
counters and traffic after recovery).

## Notes

- stop() parks TWAI and drives the transceiver into standby; main's
  sleep-prepare calls it (after ext_manager_stop, before storage).
- The ESP TWAI transceiver and the MIC3624 are two nodes on the SAME
  vehicle CAN — legal; traffic policy between autopid-via-MIC and
  native-CAN consumers is the user's call in v1.
- Memory: the can_core RX task (internal 4 KB) exists only while
  `enabled`; statics negligible.
