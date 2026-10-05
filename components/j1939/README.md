# j1939

SAE J1939: what a heavy vehicle's network says, kept as "the newest message
of every parameter group" for whoever wants to read it; and, in active mode,
a node of that network that can ask. The component subscribes to the native
CAN bus (`can_manager`) for every 29-bit frame, puts transport-protocol
messages back together and stores the newest payload per (parameter group,
source address, destination address).

**In `listen` mode (the default) it never transmits.** No address claim, no
request, no acknowledge, no flow control. It works on a bus `can_manager`
holds listen-only (`silent`), which is how a vehicle that was not built for
this device should be read.

**In `active` mode (opt-in, 2026-10-03, TASK_j1939_wwh.md phase 6) it is a
node:** it claims an address (J1939-81: the preferred `address`, 249 = off-
board diagnostic tool 1; lost to a better NAME it moves to 250, then 128 to
247, and says "cannot claim" once when every one is taken), answers a Request
for Address Claimed, negatively acknowledges requests addressed to it (it
provides no group), takes connections addressed to it (RTS/CTS, sending the
clear-to-send and end-of-message frames of a destination, an abort when the
sender breaks), and lets its readers ask the vehicle: `j1939_request()` sends
a Request for a group, the answer lands in the store or as an acknowledgment
(`j1939_request_outcome`). It needs `can_manager` in normal mode: on a
listen-only bus it stays a listener and says so once (a warning).

Readers do not get a stream: they ask for a group's newest message, one
decoded value of the built-in table, the VIN, a controller's active (DM1)
or previously active (DM2) trouble codes, and poll one sequence number to
know whether anything changed.

## Files

| File | What |
|---|---|
| `include/j1939.h` | The component's API |
| `include/j1939_core.h`, `j1939_core.c` | PURE. The 29-bit identifier, frame kinds, "is this a J1939 bus", the VIN message, the Request and Acknowledgment codec |
| `include/j1939_dm_core.h`, `j1939_dm_core.c` | PURE. DM1 / DM2 payloads: lamps, trouble codes |
| `include/j1939_spn_core.h`, `j1939_spn_core.c` | PURE. The built-in value table (21 SPNs) and its decode |
| `include/j1939_claim_core.h`, `j1939_claim_core.c` | PURE. The NAME and the address claim state machine (J1939-81): contests, moves, cannot-claim, the defence hold |
| `j1939_tp_core.h/.c` | PURE. Transport protocol reassembly as a listener (BAM, and RTS/CTS between other nodes) and as the destination of a connection (CTS / EOMA / abort replies, queued for the caller) |
| `j1939_cache_core.h/.c` | PURE. The message store and the table of sources |
| `j1939.c` | The glue: subscription, receive task, lifecycle, counters; the hooks that let active mode answer |
| `j1939_read.c` | The readers' functions (lookup + copy under the lock) |
| `j1939_tx.c` | Active mode: the claim on the bus, the answers owed, `j1939_request()` and its outcomes, the outbox that sends after the lock |
| `j1939_settings.c` | Settings descriptor |
| `j1939_http.c` | `GET /api/j1939` |
| `j1939_cli.c` | Console command `j1939` |
| `host_test/` | Host suite of the pure files (57 tests) |

## API

`#include "j1939.h"` (it brings `j1939_core.h`, `j1939_dm_core.h`,
`j1939_spn_core.h`).

| Function | What |
|---|---|
| `j1939_init()` | Register the settings and log descriptors (main's init pass) |
| `j1939_start()` | Subscribe and listen, per settings. `ESP_OK` when disabled and when the native bus is off (`J1939_STATE_NO_BUS`); `ESP_ERR_INVALID_STATE` when the settings could not be applied |
| `j1939_stop()` | Stop listening (before `can_manager_stop()`); the store keeps what it has |
| `j1939_register_http()` | The route (HTTP compositions: main wires it) |
| `j1939_status(&st)` | State, bus verdict, every counter |
| `j1939_sequence()` | A number that moves whenever a message was stored |
| `j1939_pgn_latest(pgn, sa, da, &info, buf, cap)` | The newest message of a group. `sa` / `da` may be `J1939_ADDR_ANY` |
| `j1939_spn_latest(spn, sa, &value, &raw, &info)` | One value of the built-in table, decoded from the newest message of its group |
| `j1939_vin(out, &sa)` | The VIN somebody sent (PGN 65260) |
| `j1939_dm1(sa, &lamps, codes, max, &count, &info)` | Active trouble codes of controller `sa` |
| `j1939_dm2(sa, ...)` | Its previously active codes (a group only ever answered to a request: active mode asks) |
| `j1939_active()` | Active mode AND an address held AND the bus transmit-ready: requests go out |
| `j1939_address()` | The node's address, or `J1939_ADDR_NULL` |
| `j1939_request(pgn, da)` | Send a Request for a group to one controller or `J1939_ADDR_GLOBAL`. `ESP_ERR_INVALID_STATE` in listen mode / without an address / on a listen-only bus, `ESP_FAIL` when the bus did not take the frame. The answer: the store (data), or `j1939_request_outcome` (acknowledgment) |
| `j1939_request_outcome(pgn, da, &age_ms, &control)` | `none`, `pending`, `acked`, `nacked` (control: `J1939_ACK_NEGATIVE` / `_DENIED` / `_BUSY`) for the last request of that (group, destination); 16 slots |
| `j1939_msg_next(&cursor, &info, buf, cap)` | Walk everything stored |
| `j1939_sources(out, max)`, `j1939_source(sa, &out)` | Who is on the bus (frames, age, NAME when it claimed its address) |
| `j1939_reset()` | Zero the counters, forget the stored messages |
| `j1939_capacity()`, `j1939_tp_capacity()` | Occupancy of the store and of the transport sessions (`WICAN CAPS j1939_msgs`, `j1939_tp`) |

Pure helpers a reader may want: `j1939_id_parse / _make`, `j1939_classify`,
`j1939_vin_parse`, `j1939_dm_parse`, `j1939_dtc_text` (`SPN110-0`),
`j1939_spn_table`, `j1939_spn_find`, `j1939_spn_decode`, `j1939_raw_class`,
`j1939_request_build / _parse`, `j1939_ackm_build / _parse`, `j1939_name_build
/ _compare`.

## Active mode: how the node behaves (J1939-81, J1939-21)

- **Address claim.** Once the bus lets the node transmit (`can_manager`'s
  link proven, normal mode) it sends Address Claimed (PGN 60928, priority 6,
  to everyone, data = its NAME) for the preferred address and waits 250 ms;
  nobody contesting makes the address its own (`claimed`). A contest is
  another node's claim of the same address: the lower NAME keeps it. Ours
  loses: the node moves to the next candidate (250, then 128..247 upwards)
  with a new claim and a new wait; none left: Cannot Claim Address (the
  claim message from the null address 254) once, and silence
  (`cannot_claim`). Ours wins: the claim is re-sent (a defence), but at most
  once per 250 ms per address: a broken node that contests every claim it
  hears (the bench truck's `--contend` mode did 150 rounds in 2 s before the
  hold) is counted (`held`) and not answered again. A Request for Address
  Claimed, to us or to everyone, is answered with the claim (or the
  cannot-claim) at any time.
- **NAME.** Identity number = the device-specific 21 bits of its MAC,
  manufacturer code 0 (none assigned), function 129 (off-board diagnostic
  service tool), industry group 0, arbitrary address capable. Shown on
  `/api/j1939` and the console, most significant byte first.
- **Requests addressed to us** for a group we do not provide get a negative
  acknowledgment (PGN 59392, to everyone, naming the requester), as J1939-21
  asks; global requests are never NACKed. We provide no group.
- **Connections addressed to us** (a controller answering our request for a
  long group with RTS): we answer CTS for the window the sender allows (RTS
  byte 4), take the packets, send the end-of-message acknowledge, and abort
  (reasons 1 no session, 3 timeout, 7 bad sequence) when it goes wrong.
  Without an address nothing is answered: the node stays the listener of
  phase 4, following connections between others.
- **Nothing is sent from under the lock.** Frames are decided by the
  receive task while it sorts the frames that call for an answer (and by its
  clock, every loop) into an outbox, and sent after the lock is released;
  `j1939_request()` sends from the caller's task. Every transmit goes through
  `can_manager_send()`, which refuses while the bus is not transmit-ready.
- **The bus flooded** (phase 6 bench: 1000 priority-0 frames/s): the
  controller is single shot, a frame that loses arbitration is lost at the
  hardware; `can_manager`'s owned TX slots retry it from the tx_done
  interrupt (`/api/can tx_retries`): 20 of 20 requests answered, 6 retries,
  nothing lost.

**The pick.** With `J1939_ADDR_ANY` as the source and several controllers
sending the group: the lowest address heard within the last 5 s (the engine
is 0, and a pick must not flip between sources from one read to the next);
when nobody was that recent, the lowest address of all, so a bus that falls
silent does not move the pick to whoever happened to speak last (until
2026-10-03 it did, and an autopid row published that controller's stale
message as news five seconds into the silence). A reader that needs one
controller names it.

**Who reads the store.** autopid (TASK_j1939_wwh.md phase 5): a PID row whose
command reads `PGN:<hex>[@<source>][?]` is served from here
(`j1939_pgn_latest`), its detection job and first contact read the sources,
the VIN and the groups heard (`j1939_sources`, `j1939_vin`, `j1939_pgn_latest`
over the built-in table), its DTC report folds in `j1939_dm1` of every
source; the Quick Setup enables this component together with `can_manager`
when a J1939 network is heard. Nothing here knows autopid.

**Not available is not a value.** J1939 marks the top of every raw range:
`FF` = the sender has no such parameter, `FE` = it cannot measure it now,
`FB` = a parameter specific indicator (the gear's "park"). `j1939_spn_latest`
reports which (`j1939_raw_t`) and writes a value only for a valid raw. The
table's `max` is the largest valid raw value decoded, so a consumer that
clamps to `[min, max]` drops the rest by itself.

**Trouble codes.** `SPN<number>-<FMI>`, the form the rest of the firmware
uses. A record with the conversion method bit set (an SPN layout older than
1996's version 4, which the message does not tell apart) is decoded as
version 4 and flagged (`cm`): nothing is guessed.

## The built-in values

Names are the SPN labels, and none equals a name of autopid's OBD table (a
vehicle may carry both sets). Everything else comes from a DBC file or a
custom row, in autopid.

| Name | SPN | Group | Unit |
|---|---|---|---|
| EngineSpeed | 190 | F004 EEC1 | rpm |
| ActualEnginePercentTorque | 513 | F004 | % |
| AccelPedalPosition1 | 91 | F003 EEC2 | % |
| EnginePercentLoad | 92 | F003 | % |
| TransCurrentGear | 523 | F005 ETC2 | |
| WheelBasedVehicleSpeed | 84 | FEF1 CCVS1 | km/h |
| EngineCoolantTemperature | 110 | FEEE ET1 | degC |
| EngineOilTemperature | 175 | FEEE | degC |
| EngineOilPressure | 100 | FEEF EFL/P1 | kPa |
| IntakeManifoldPressure | 102 | FEF6 IC1 | kPa |
| IntakeManifoldTemperature | 105 | FEF6 | degC |
| BarometricPressure | 108 | FEF5 AMB | kPa |
| AmbientAirTemperature | 171 | FEF5 | degC |
| BatteryPotential | 168 | FEF7 VEP1 | V |
| FuelRate | 183 | FEF2 LFE1 | L/h |
| FuelLevel1 | 96 | FEFC DD1 | % |
| DEFTankLevel | 1761 | FE56 AT1T1I | % |
| TotalVehicleDistanceHR | 917 | FEC1 VDHR | km |
| TotalVehicleDistance | 245 | FEE0 VD (on request) | km |
| EngineTotalHours | 247 | FEE5 HOURS (on request) | hours |
| EngineTotalFuelUsed | 250 | FEE9 LFC (on request) | L |

The three on-request groups are seen only when some other node of the
vehicle asks for them (an instrument cluster usually does): a listener
cannot ask. In active mode an autopid row marked `?` (`PGN:FEE5?`) asks for
its group at its period (once a second at most), and the detection asks for
the VIN (PGN 65260) once.

## Counters: nothing fails silently

`j1939_status()`, `GET /api/j1939`, console `j1939`.

- Every frame taken from the bus is in exactly one of `rx_data` (a parameter
  group in a frame: stored), `rx_tp_cm`, `rx_tp_dt` (transport protocol),
  `rx_diag` (ISO 15765 on J1939 identifiers, groups DA00 / DB00 / CD00 /
  CE00: somebody's OBD or UDS conversation, not stored), `rx_foreign` (not
  J1939). `rx_frames` is their sum.
- `queue_drops`: frames lost before the component saw them, because its
  queue (512 frames) was full. `can_manager` counts them per subscriber.
- Transport protocol: every announce is counted (`tp_started`,
  `tp_no_session`, `tp_bad_cm`); every started session ends as exactly one
  of `tp_completed`, `tp_seq_errors` (a packet out of sequence: by
  listening, a gap cannot be filled), `tp_timeouts` (1.25 s without a
  frame), `tp_aborted`, `tp_replaced` (a new announce from the same sender);
  a data packet that belongs to no session is a `tp_orphan_dt`.
- The store is bounded (384 entries, 12 long buffers of 1785 bytes): a new
  key takes the place of the entry silent the longest when that one has
  been silent for 30 s (`evicted`), otherwise the message is not kept
  (`not_kept`); when every long buffer is taken, the holder silent the
  longest loses its payload (`long_evicted`).
- Active mode (`claim`, `tx` on `/api/j1939`): `claims_sent`, `contests`
  (= `won` + `lost`), `held` (won, not answered again), `requests_answered`,
  `cannot`; `tx.frames` / `tx.failed` (the bus refused: not ready, or the
  outbox of 16 was full), `requests`, `acks`, `nacks` (ours answered),
  `nacks_sent` (theirs answered), `tp_to_me`, `tp_cts`, `tp_eoma`,
  `tp_aborts`, `tp_reply_lost` (the reply queue of 8 was full).

The conservation bench (below) holds these to the frame.

## Dependencies

`PRIV_REQUIRES settings_manager log_manager can_manager cmdline_manager
console http_server_manager esp_http_server esp_timer`, managed
`espressif/cjson` (the settings hook's argument type).

Init order: after `can_manager_init()`. Start order: after
`can_manager_start()` (it subscribes to the running bus) and before its
readers. Stop order: before `can_manager_stop()`.

The bus has to be up for it: `can_manager.enabled`. With the native bus off
`j1939_start()` returns `ESP_OK`, logs one warning and the state is
`no_bus`. For a vehicle network: `can_manager` with `baud: "auto"` and
`silent: true` (listen mode) or `silent: false` (active mode: the node must
acknowledge and transmit).

## Settings

Component `j1939`, schema version 1, reboot-to-apply. `mode` and `address`
were added 2026-10-03 as non-breaking additions (a stored file without them
gets the defaults at boot).

| Key | Default | What |
|---|---|---|
| `enabled` | `false` | Listen on the native CAN bus |
| `mode` | `listen` | `listen`: never a frame. `active`: claim an address, answer, ask, clear |
| `address` | `249` | The address claimed first in active mode (128..253; 249 / 250 are the diagnostic tool addresses) |
| `cli` | `true` | Register the `j1939` console command |

## HTTP: `GET /api/j1939`

One route, four views.

**`/api/j1939`**: state, mode, the address claim and the transmit counters,
counters, sources, values, trouble codes.

```json
{"enabled":true,"state":"listening","bus":"j1939","mode":"active",
 "claim":{"state":"claimed","address":249,"preferred":249,"tx_ready":true,
          "name":"3C651A0000810080","claims_sent":1,"contests":0,"won":0,
          "lost":0,"held":0,"requests_answered":0,"cannot":0},
 "tx":{"frames":7,"failed":0,"requests":4,"acks":0,"nacks":1,"nacks_sent":0,
       "tp_to_me":1,"tp_cts":1,"tp_eoma":1,"tp_aborts":0,"tp_reply_lost":0},
 "vin":"1WCANJ1939TRUCK01","vin_sa":0,
 "can":{"running":true,"baud_kbps":250,"link":"running","listen_only":false},
 "stats":{"rx_frames":2236,"rx_data":2194,"rx_tp_cm":14,"rx_tp_dt":28,
          "rx_diag":0,"rx_foreign":0,"queue_drops":0,"messages":2208,
          "not_kept":0,"evicted":0,"long_evicted":0,"entries":15,
          "entries_max":384,"seq":2208},
 "tp":{"open":0,"max":8,"started":14,"completed":14,"seq_errors":0,
       "timeouts":0,"aborted":0,"replaced":0,"no_session":0,"orphan_dt":0,
       "bad_cm":0},
 "values":[{"name":"EngineSpeed","spn":190,"pgn":"F004","sa":0,
            "state":"valid","value":1500,"unit":"rpm","age_ms":10,
            "period_ms":20}],
 "sources":[{"sa":0,"frames":1411,"age_ms":15,"name":"B3A2612400000000"}],
 "dm1":[{"sa":0,"age_ms":184,"mil":1,"rsl":0,"awl":1,"pl":0,"count":3,
         "dtcs":[{"code":"SPN110-0","spn":110,"fmi":0,"oc":5,"cm":false}]}]}
```

- `state`: `off` (disabled), `no_bus` (enabled, native CAN off), `listening`
  (in both modes: active mode listens too).
- `mode`: the setting. `claim.state`: `idle` (listen mode, or the bus not
  transmit-ready yet), `claiming`, `claimed`, `cannot_claim`;
  `claim.address` 254 while none is held; `claim.name` the NAME, most
  significant byte first.
- `bus`: `unknown`, `j1939` (two distinct well-known groups were seen),
  `other` (50 frames and none of them).
- `vin`: `null` until a valid VIN message was seen.
- `values`: only the table entries whose group somebody sends. `state` is
  `valid`, `na`, `error`, `specific`, `reserved` or `short`; `value` is
  present only with `valid`. `sa` is the source the pick chose.
- `sources[].name`: the NAME of its address claim, hex as sent, or `null`.
- `dm1`: one entry per controller that sent DM1; lamps 0 = off, 1 = on,
  2 = error, 3 = not available; `count` may exceed the 32 codes listed.

**`/api/j1939?pgns=1`**: everything stored.

```json
{"seq":2208,"pgns":[{"pgn":"F004","sa":0,"da":255,"len":8,"count":798,
  "period_ms":18,"age_ms":12,"data":"F0AAA5E02EFFFFFF"}]}
```

`pgn` is hex, `sa` / `da` decimal (`da` 255 = broadcast), `data` the whole
payload in hex (up to 1785 bytes), `len` 0 = a long payload that had to make
room.

**`/api/j1939?pgn=FEEC[&sa=0][&da=255]`**: one group's newest message, in
the shape of a `pgns` element; `sa` / `da` decimal or `0x..`; without them
the pick applies. `404 {"error":"nobody sent this group"}` otherwise.

**`/api/j1939?request=FECB[&da=0]`** (active mode): send a Request for that
group to one controller (`da`, default everyone) and answer at once:

```json
{"sent":true,"pgn":"FECB","da":0,"from":249,"outcome":"pending"}
```

The data arrives in the store a moment later (`?pgn=FECB`); an acknowledgment
replaces `outcome` on the next request of the same (group, destination).
`409 {"error":"listen mode: ..."}` / `"no address on the bus yet ..."`, `503`
when the bus did not take the frame.

## Console

`j1939` (state, mode, bus verdict, counters, sources; in active mode the
address claim and the transmit counters), `j1939 -s` (the built-in values
the bus carries), `j1939 -d` (DM1 per controller), `j1939 -p` (every stored
message, first 8 bytes), `j1939 -r <pgn hex> [da hex]` (active mode: send a
Request), `j1939 -z` (zero the counters, forget the stored messages).

## Memory footprint

Measured on the build of 2026-10-03 (`xtensa-esp32s3-elf-size` on
`libj1939.a`, task watermark from `GET /api/status/tasks` on the bench).

| What | Where | Bytes |
|---|---|---|
| Code + constants | flash | 17.4 KB text, 5.0 KB rodata |
| Static: task control block, queue and mutex objects, counters | internal RAM | 621 |
| Active mode (phase 6): the claim, the outbox (16 frames), the request slots (16), counters | internal RAM | about 620 (by reading the code: the statics of `j1939_tx.c`) |
| Message store (512 slots, 12 long buffers) | PSRAM `.ext_ram.bss` | 41 944 |
| Transport sessions (8 x 1785 bytes) | PSRAM | 14 512 |
| Frame queue (512 frames) | PSRAM | 10 240 |
| Sources (256 rows) | PSRAM | 5 152 |
| Receive task stack (1 task, priority 7) | PSRAM | 4 096; 3 244 never used on the bench (measured) |
| Heap, per `GET /api/j1939` | PSRAM | 3 833, freed with the reply (measured by reading the code: one block) |

Total 75.9 KB of PSRAM, allocated whether the component is enabled or not
(static allocation, Standard §2). No flash writes; the settings file is
written once, at the first boot.

## Tests

- **Host**: `.\test.ps1 host j1939`, 57 tests over the six pure files (see
  `host_test/README.md`): 43 of the listener, 14 of active mode (the NAME,
  the claim's wait / contest won / lost / out of addresses / request for
  address claimed / defence hold, the request and acknowledgment codec, the
  transport protocol as destination: CTS, windows of one packet, EOMA, the
  three aborts, no address = no reply, the reply queue bound). The frames
  and payloads are the ones the bench's reference codec produces.
- **Bench** (`.\test.ps1 j1939` in the firmware repo, the PCAN adapter as a
  truck): `tools/testbench/can/j1939_bench.py` -> `J1939 PASS` (every value
  and payload against the truck's, BAM VIN, DM1 with three / one / no code,
  staleness, not available and error, broken BAMs, 250k and 500k found by
  listening, normal mode, the native bus off; the DUT's `tx` stays 0 and the
  adapter sees no frame but its own), then
  `tools/testbench/can/j1939_conservation_test.py` -> `J1939 CONSERVATION
  PASS` (exactly N frames of four keys at 1000 frames/s, 80 % load and line
  rate, 250k and 500k, twice each: every key's count moves by its share, or
  the named loss counters account for the rest), then the autopid stage, then
  `tools/testbench/can/j1939_active_bench.py` -> `J1939 ACTIVE PASS` (the
  claim as the truck saw it; a single-frame answer, the VIN by BAM, DM2 by
  RTS/CTS with our CTS / EOMA, a NACK counted; the truck's requests for our
  address and for a group we do not have; a contest kept and defended once
  per round, a contest lost and the move to 250; a priority-0 flood with
  every request answered and the retries counted; autopid's `?` rows, DM2 in
  the DTC report and the DM11 / DM3 clear; a J2534 tester's diagnostics hold
  stopping the asking; listen mode and a listen-only bus with not one frame).
- **End to end** (`.\test.ps1 eutruck`):
  `tools/testbench/obd/eu_truck_e2e_bench.py` -> `EU TRUCK E2E PASS`: the
  listener beside the OBD chip on an EU truck (WWH-OBD ECUs and a J1939
  network on one bus, one VIN): the network found by a bus sample while the
  native bus is off, the listener started by the one restart the Quick Setup
  stages (listen-only at the measured bit rate), its rows live beside the
  chip's `22F4xx` rows, DM1 in the same DTC report as the `19 42` codes and
  still heard after the legislated clear (a listener clears nothing).
