> **App developers (Android/iOS/desktop): the on-air contract lives in**
> **[`BLE_API.md`](BLE_API.md)** — advertising, pairing, every service/
> characteristic UUID with byte-exact values, pipe semantics, and the
> WiFi-handover behavior an app must expect (2026-07-05).



## GATT backend: Bluedroid vs NimBLE (switchable, 2026-07-05)

The stack-facing layer is behind the `blm_gatt_*` contract, with two
implementations selected by the sdkconfig BT host choice (CMake picks
the file): `ble_manager_gatt.c` (Bluedroid) or `ble_manager_gatt_nimble.c`
(NimBLE, `CONFIG_BT_NIMBLE_ENABLED` + `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_
EXTERNAL` so host allocs ride PSRAM). Same on-air contract either way --
UUIDs, byte-exact Device Info (incl. legacy NUL/padding), SC+MITM+BOND
static passkey, MTU 517, adv layout, conn-window request. Verified with
`tools/testbench/ble_ab.py` (pair, Device Info, CLI RTT, both pipes
through a `ble <-> obd0` bridge).

Measured on the composed main (WiFi STA+AP + mutual-TLS MQTT active --
coexistence throttles BLE in these numbers; the earlier 4.6 ms RTT bench
ran WiFi-off):

| | Bluedroid | NimBLE (EXTERNAL) |
|---|---|---|
| BLE start, internal RAM | 58,408 | **41,644** (-16.8K) |
| internal free after boot | 79,115 | **90,031** (+10.9K net) |
| firmware size | 1.98 MB | **1.80 MB** (-183K) |
| CLI RTT p50 (coex-loaded) | 226 ms | 390 ms |
| TX notify (coex-loaded) | 20.5 KB/s | 21.7 KB/s |
| pair / Device Info / pipes | PASS | PASS |

NimBLE mapping notes (in the file header too): no controller-credit API --
notify does a bounded ENOMEM retry feeding the congestion flag; no
per-CCCD permissions -- notify additionally refuses until the link is
secured; long CLI writes arrive reassembled.

**WiFi-state sweep (2026-07-05, meatpi asked)** -- CLI RTT p50, both
stacks, same bench:

| WiFi state | Bluedroid | NimBLE |
|---|---|---|
| APSTA + MQTT (first runs) | 226 ms | 390 ms |
| STA-only | 390 ms | 390 ms |
| WiFi OFF (`wifi --stop`; BLE is the sole radio user) | **195.0 ms** | **195.0 ms** |

Conclusion: the stacks are IDENTICAL in every condition (the day-one
226-vs-390 was run-to-run environment, not the stack). WiFi coex costs
~2x CLI RTT (195 -> 390 ms); the 195 ms radio-off floor is the bench's
write-with-response chaining across BlueZ connection events, not
firmware (the historic 4.6 ms figure measured the FFF2->FFF1 echo path
with the WiFi-off test app). `wifi --stop` (ephemeral, reboot restores)
exists for exactly this kind of isolation run.

**Re-run on the CLEAN bench (2026-07-05, TP-Link UB500 dongle — the
Pi's internal adapter, disabled since, was inflating every earlier RTT
number ~3x):**

| metric | Bluedroid | NimBLE (EXTERNAL) |
|---|---|---|
| CLI RTT p50, interface_manager suspends WiFi (product path) | 76.6 ms | 76.2 ms |
| CLI RTT p50, WiFi forced on (rules off, DUT coex) | 113.0 ms | 115.3 / 117.0 ms |
| TX notify, coex-loaded | 28.2 KB/s | 24.0 / 28.6 KB/s (par - RF run-to-run band) |

Same verdict, now on clean data: performance identical; NimBLE keeps
the default slot on memory (-16.8K internal) and flash (-183K).

## Throughput vs the esp-idf ble_throughput demo (2026-07-05)

meatpi: "the esp32 throughput BLE demo shows better numbers". Ladder
run on the clean bench (NimBLE, UB500), tuning one lever at a time:

| configuration | TX notify | CLI RTT p50 |
|---|---|---|
| product (`ios` profile), WiFi coex forced on (rules off) | 24-28.6 KB/s | 115 ms |
| + DLE forced (`ble_gap_set_data_len` 251) | **2.1 KB/s — collapse** | unstable connects |
| `android_fast` profile (15 ms interval), coex | 26.1 KB/s | 66.3 ms |
| **clean radio** (rules ON -> BLE connect suspends WiFi), `blast` over UART | **77.1 KB/s** | — |

Conclusions:

- **The gap is coex, not the stack.** With WiFi suspended (which IS the
  product path — interface_manager suspends STA/AP on a BLE connect)
  the same firmware does 77 KB/s, demo territory. The demo runs with
  no WiFi at all.
- **Connection interval helps RTT only** (115 -> 66 ms with
  `android_fast`); TX is airtime-bound under coex, so the interval
  lever doesn't move it.
- **DLE is harmful under coex** — forcing 251-byte LL packets collapsed
  TX 28 -> 2 KB/s and destabilised connects (long LL packets lose the
  coex arbitration). Reverted; do-not-retry note lives in
  `ble_manager_gatt_nimble.c`.
- Measurement notes: `blast N` (main_cli.c) pumps N bytes through
  `ble_manager_send`; fire it over **UART only** — BLE CLI lines run
  inline in the NimBLE host task, so a long command wedges the stack
  (GATT Unlikely Error; fix item filed: route BLE CLI through the
  cmdline dispatcher queue). Pi side: `tools/testbench/ble_blast.py`
  (passive subscribe + count). NimBLE logs an INFO pair per notify —
  console spam during blast, harmless.
