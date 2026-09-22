> **App developers (Android/iOS/desktop): the on-air contract lives in**
> **[`BLE_API.md`](BLE_API.md)** — advertising, pairing, every service/
> characteristic UUID with byte-exact values, pipe semantics, and the
> WiFi-handover behavior an app must expect (2026-07-05). The HTTP API
> and J2534 ride on stream channels: `ble_http/BLE_HTTP_PROTOCOL.md`,
> `j2534_server/J2534_WIRE_PROTOCOL.md` (2026-09-21).

## Stream channels (2026-09-21)

Besides the legacy data pipe (FFF1/FFF2, the bridge_manager `ble` jack)
and the CLI pipe, `ble_manager` hosts a bounded registry of **stream
channels**: a consumer registers `{name, uuid_out, uuid_in, rx_storage,
rx_size, on_event}` BEFORE `ble_manager_start()` (the GATT table is built
from the registry at start; `ble_manager_channel_lock()` freezes it) and
gets one notify + one write characteristic APPENDED to the FFF0 service
(handles of the legacy four stay stable for apps with a cached GATT
database). Semantics: a reliable in-order byte stream, no framing here.

| Function | Contract |
|---|---|
| `ble_manager_channel_register(desc, &id)` | pre-start only; 16-bit UUIDs on the FFF0 base (odd out, even in); refuses duplicates / FFF1 / FFF2; `ESP_ERR_NO_MEM` + `E` when the registry (cap 4) is full (§12) |
| `ble_manager_channel_read(id, buf, n, timeout_ms)` | `>0` bytes, `0` timeout, `<0` link down or unknown id. Leftovers of a dropped link are discarded on the first read after it (a link generation counter; the reader resets its own StreamBuffer, which is the only legal place). 100 ms slices bound link-down detection. This is the `j2534_transport_t.read` contract verbatim |
| `ble_manager_channel_write(id, buf, n)` | MTU-chunked PDUs in the OUT mode the central selected with its CCCD among the owner's `out_modes` (`ble_manager_channel_out_mode`): **indications** (`blm_gatt_indicate_handle`: one in flight per link, each confirmed within `BLM_CHANNEL_TX_WAIT_MS` 30 s = the ATT indication timeout; a phone confirms only after the writes it already queued went out, so a 2 s budget stalled uploads) or **notifications** (`blm_gatt_notify_handle`, bounded ENOMEM retry inside the same 30 s, VHCI ingress gate; 2026-09-22, opt-in per channel because a notification has no end-to-end acknowledgement, so the owner's protocol must carry credits + a frame counter: ble_http v2 does, ble_j2534 stays indicate-only); `n`, `-1` no secured link, `-2` timeout. Never from the BT host task. Counters `tx_notifications` / `tx_indications`, the live mode in `GET /api/ble` `channels[].out` |
| `ble_manager_channel_stats/capacity` | counters (`rx_overflow`, `tx_timeouts`, ...) and the `WICAN CAPS ble_ch=used/cap` line |

Events (`on_event`, BT host task, flag-and-wake only): `CONNECTED`
(unusable yet), `SECURED` (bytes flow), `DISCONNECTED`. Writes on a
channel are refused until the link is secured (`WRITE_ENC|WRITE_AUTHEN`
plus the software check). A full RX buffer answers *Insufficient
Resources* to write-with-response and counts `rx_overflow` for
write-without-response; the owner's protocol carries the flow control
(ble_http: CREDIT frames, both directions in notify mode; j2534: one
request pipelined at most). The GATT properties of an OUT characteristic
follow the descriptor's `out_modes` (0 = indicate only, the pre-2026-09-22
contract); the GAP `SUBSCRIBE` event (`blm_channel_on_subscribe`) records
the central's CCCD per channel, reset on disconnect (a bonded central's
CCCDs come back as `SUBSCRIBE reason=RESTORE`). The pure picker
`blm_channel_pick_out` (notify wins a `0x0003`, unsubscribed = indicate)
is host-tested. **The notification loss of 2026-09-21 (2 of 146 PDUs
counted by the host, never on air) was the CONTROLLER, not NimBLE**: with
`CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=0` the ESP32-S3 controller mallocs
each ACL TX buffer from the internal heap at transmit time and drops the PDU
silently when that fails, which on a device booting with ~8 KB free happened
in 7 of 24 notify downloads (btmon arbiter, 1-4 consecutive PDUs each).
`CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=12` (the Kconfig maximum, ~3 KB
internal at controller init) made 0 of 12 lose a PDU; gating on
`esp_vhci_host_check_send_available()` and 32/48 NimBLE mbuf blocks (PSRAM,
kept) did not. `ble_manager_gatt_tx.c` holds the notify/indicate primitives
(split out for the 700-line rule). **Cost:** the 12 static buffers take
3.2 KB of internal RAM at controller init (`WICAN MEM internal_free` at boot
with BLE on: 7991 -> 4787). At that level the bench DUT ran into the
internal-RAM cliff within minutes (`E httpd: Failed to post
esp_http_server event: ESP_ERR_TIMEOUT` every 2 s, the tunnel's loopback
requests failing with 502, `RAM Min free ever: 155 bytes`), so the bench DUT
now runs `wifi_manager.wifi_ram_profile = lean` (boot free 18403, +13.6 KB;
its own README recommends lean for WiFi + BLE). Any device that ships with
BLE on needs the same, or a smaller static count re-measured for drops.

Files: `ble_manager_channel.c` (registry, stack-agnostic),
`ble_manager_gatt_svc.c` (the NimBLE service table + access callbacks,
split out of `ble_manager_gatt_nimble.c` which keeps GAP/SM/adv/notify).
NimBLE only: the Bluedroid A/B backend stubs `blm_gatt_notify_handle()`
with `ESP_ERR_NOT_SUPPORTED`. `CONFIG_BT_NIMBLE_MAX_CCCDS` is 16 (5
notify characteristics x 3 bonds; the Kconfig default 8 would lose
subscriptions).

Memory (estimated): registry + 4 `StaticStreamBuffer_t` ~0.7 KB internal
`.bss`, two indication semaphores ~180 B internal; the mutable FFF0 table
+ UUIDs + a 490 B RX scratch in PSRAM; the RX storage belongs to the
consumer (PSRAM). `GET /api/ble` (`ble_manager_http.c`) reports the link
and every channel's counters.

**Congestion fix (2026-09-21).** The former `s_congested` latch in
`notify_handle` was set on ENOMEM and cleared only by a successful
notify, while the IO layer's TX task never notified while it was set:
one overload (the slcan bench's 1000 fps flood) killed BLE TX for the
rest of the boot. It is now a self-expiring 5 ms window
(`s_congested_until`) that `blm_gatt_notify_handle()` feeds while it
retries with a bounded deadline (data/CLI pipes 100 ms, channels 2 s), and
it resets on connect / disconnect / host reset. The `E` line in the TX
task's pacing loop is a `D` (a data-path condition, §10). Regression
witness: `ble_slcan_bridge_test.py --flood` leg E.

## BLE 5 as settings: PHY + advertising sets (2026-09-21)

Descriptor v4 adds `phy` (`1m` default | `2m` | `coded` | `auto`) and
`advertising` (`legacy` default | `extended` | `both`); v3 configs migrate
by filling the defaults (`on_migrate` = fill-missing, pure). Design and
measurements: `TASK_ble5_phy_adv.md`. NimBLE's 5.0 feature set was already
compiled in (`CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT`); the change compiles
extended advertising in once (`CONFIG_BT_NIMBLE_EXT_ADV=y`, 2 instances,
256 B max) and chooses at runtime:

| Piece | Where | Behaviour |
|---|---|---|
| advertising sets | `ble_manager_gatt_adv.c` (`blm_adv_configure/start/stop`) | instance 0 = the legacy PDU set (today's bytes), instance 1 = the extended set (flags + UUID + name + `MeatPi` in one PDU, connectable, primary 1M, secondary per `phy`). With `BLE_EXT_ADV` on NimBLE turns `ble_gap_adv_start()` into an ENOTSUP stub, so the legacy set also goes through the extended API (`legacy_pdu = 1`). All instances stop on connect (one central), the configured ones restart on disconnect / failed connect |
| PHY | `ble_manager_gatt_nimble.c` | `ble_gap_set_prefered_default_le_phy(mask, mask)` in `on_sync` for EVERY value (`1m` = 0x01 keeps a 2M-asking central on 1M; the controller itself starts the switch for the others); a host-side `ble_gap_set_prefered_le_phy()` follows on CONN_UPDATE / ENC_CHANGE only when the controller did not already switch; `BLE_GAP_EVENT_PHY_UPDATE_COMPLETE` logged at I (W when refused, never E); `blm_gatt_phy()` reads the live PHYs for `GET /api/ble` (`phy_tx`/`phy_rx`) |
| no Security Request at connect | `ble_manager_gatt_nimble.c` CONNECT | the legacy `esp_ble_set_encryption`-on-connect mirror is gone (2026-09-21): phones pair on first encrypted access anyway, and the peripheral-driven start broke Windows apps' custom pairing (WinRT `Failed`, no ceremony; the DUT saw ENOTCONN 2 s in). `pairing failed` now logs the SMP status at W |
| advertising TX-power AD | `ble_manager_gatt_adv.c` | the configured level, not `BLE_HS_ADV_TX_PWR_LVL_AUTO`: AUTO issues the legacy LE_Read_Advertising_Channel_Tx_Power HCI command, which the controller answers *Command Disallowed* once it runs the extended API (the first bench boot advertised nothing) |
| pure mapping | `ble_manager_pack.c` (`blm_ident_phy_mask`, `blm_ident_adv_mode`), host-tested | `1m`->0x01, `2m`->0x02, `coded`->0x04, `auto`->0x03, unknown->1M; `legacy`/`extended`/`both`, unknown->legacy |

Memory: extended advertising compiled in = +1.8 KB flash, 0 B static
internal RAM (measured 2026-09-21, `idf.py size` before/after; NimBLE's
allocations are in PSRAM here); the runtime instances add nothing while
`advertising = legacy`. The Bluedroid A/B backend ignores both settings
(1M, legacy) and reports `phy_tx/rx = 1` while connected.

## Bench blast route (2026-09-21)

`POST /api/ble/blast {"bytes":N,"chunk":490}` (`ble_manager_http.c`) is the
July `blast` console command as an HTTP route: a PSRAM-stack task pumps N
bytes of a counting pattern through `ble_manager_send()` onto the data pipe
(FFF1 notifications), yielding 2 ms whenever the TX queue is full, and
`GET /api/ble` reports `blast:{running,bytes,target,ms}`. Because it is an
`/api/*` route it is reachable through the BLE tunnel, so a central can
start it with WiFi handed over: the `notify` mode of `ble_central_bench`
does exactly that and counts what arrives. Not a product feature; a bench
source (one at a time, 409 while running).

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
