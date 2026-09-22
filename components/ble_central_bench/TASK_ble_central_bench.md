# TASK: the ECU simulator as the BLE test central (`ble_central_bench`), 2026-09-21

## Why

Ali: the bench needs a BLE central we control end to end, instead of the
Pi's UB500 (cannot hold a 2M link with the S3) and the PC's Windows stack
(no timing control). Reference to match: esp-idf
`examples/bluetooth/nimble/throughput_app` (README, 60 s test, MTU 512,
connection interval 7.5 ms, DLE 251, 1M PHY, two ESP32 boards):

| GATT operation | reference |
|---|---|
| NOTIFY (peripheral -> central, 509 B payloads) | ~340 kbps = 42.5 KB/s |
| WRITE (write-without-response, central -> peripheral) | ~500 kbps = 62.5 KB/s |
| READ (510 B reads) | ~200 kbps = 25 KB/s |

How the example gets there (both sides): `ble_att_set_preferred_mtu(512)`
+ `ble_gattc_exchange_mtu`, `ble_hs_hci_util_set_data_len(conn, 251, 2120)`
right after connect, `ble_gap_update_params` 6/6 units (7.5 ms), latency 0,
supervision 600, ce_len 12..24; peripheral notify loop with a pipeline of
15 notifications in flight and a retry on ENOMEM (`vTaskDelay(1)`), so no
PDU is dropped; `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=50`,
`TRANSPORT_ACL_FROM_LL_COUNT=20`, `TRANSPORT_EVT_SIZE=255`.

Our July data point on the same class of path (WiCAN `blast` notifications
on the data pipe, clean radio, UB500 counting): 77 KB/s = 616 kbps, above
the reference already. The paths that are slow today are the stream
channels' OUT direction (indications, one in flight: 5.6 KB/s) and the
Pi's write path (D-Bus per write). This task gives us the instrument to
measure every path properly and a peripheral-side blast that is driven
over BLE itself.

## Blocker to raise first

The simulator's UART is not wired to any CH344 channel (probed A/C/D:
silence) and its USB is the app's composite device (NCM + CDC), so the
first reflash needs Ali: hold BOOT through a reset so the ROM exposes
USB-Serial-JTAG on the same USB-C, then `build_flash.ps1 -port COMx`; or
wire the UART header to a free CH344 channel. After that first flash an
OTA route can be added so later builds go over USB-NCM.

## Contract

### New component `ble_central_bench` (meatpi-components, `visibility: open`)

A NimBLE **central** that connects to one `WiC_*` peripheral, tunes the
link like the reference, pairs with the fixed passkey, and runs timed
GATT throughput tests. Reusable by any MeatPi bench device; first user is
the ECU simulator firmware.

Settings `ble_central_bench` v1 (reboot-to-apply):

| Key | Default | Meaning |
|---|---|---|
| `enabled` | false | run the BLE central at all (the simulator ships with it off) |
| `target` | `WiC_` | advertised-name prefix to connect to (or a full name) |
| `passkey` | 123456 | the peripheral's fixed passkey, injected on `PASSKEY_ACTION INPUT` |
| `mtu` | 517 | preferred ATT MTU |
| `conn_itvl_units` | 6 | connection interval in 1.25 ms units (6 = 7.5 ms) |
| `ce_len_units` | 24 | max connection event length in 0.625 ms units |
| `dle` | true | request 251 B LL packets after connect |
| `phy` | `1m` | `1m` / `2m` preference set after connect |
| `cli` | true | `blebench` console command |

HTTP (`ble_central_bench_register_http()`):

| Route | Method | Behaviour |
|---|---|---|
| `/api/ble_bench` | GET | `{"state":"idle|scanning|connecting|connected|running","peer":{"name","addr","mtu","itvl_ms","phy_tx","phy_rx","dle_tx","dle_rx","secured"},"chars":{"fff1","fff2","fff3","fff4","dis"},"last":{"mode","bytes","ms","kbps","count","errors","ok"},"counters":{...}}` |
| `/api/ble_bench` | POST | `{"action":"connect"}` (scan + connect + pair), `{"action":"disconnect"}`, `{"action":"run","mode":"notify|write|read|tunnel_down|tunnel_up","seconds":10,"size":65536}` (starts a test on the connected link; results in GET `last`) |

Modes:

- `notify`: subscribe to FFF1 (data pipe) notifications, ask the peripheral
  to blast through the tunnel (`POST /api/ble/blast {"bytes":N}` on FFF3/4,
  WP3) and count bytes/PDUs until the byte count or the timeout. Reports
  kbps like the reference's NOTIFY figure.
- `write`: write-without-response to FFF2 (data pipe IN) as fast as the
  stack accepts for `seconds`; the WiCAN counts on its side (`/api/ble`
  after the link, or the ring). Reports kbps like WRITE.
- `read`: repeated reads of the Device Information manufacturer string
  (2A29) for `seconds`; reports kbps like READ (bounded by the 6-byte
  value; the reference reads 510 B, so this leg is reported, not matched).
- `tunnel_down` / `tunnel_up`: the HTTP-over-BLE client on FFF3/FFF4
  (pure codec in `ble_central_bench_tunnel.c`, host-tested): upload
  `size` bytes to `/api/fs/upload?path=/data/blebench.bin` honouring
  CREDIT, download it back byte-exact. The app-level numbers Ali sees.

CLI `blebench` (status | connect | disconnect | run <mode> [seconds] [size]).
Logs: I on connect/PHY/MTU/result, W on refused procedures, never E for a
link event (§10).

### Simulator firmware (`wican-ecu-sim-fw`)

- `sdkconfig.defaults`: `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`,
  central + observer roles only, `CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y`,
  `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517`,
  `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=50`,
  `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=20`,
  `CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE=255`, NVS bond store.
- `CMakeLists.txt`: `EXTRA_COMPONENT_DIRS` = the one meatpi-components
  directory (the simulator otherwise carries its own July copies of the
  core components; the new component uses only the shared surface:
  `SETTINGS_*` field tables, `settings_descriptor_t`,
  `cmdline_manager_register`, `http_server_manager_register_handlers`,
  `log_descriptor_t`).
- `main.c`: init after `cmdline_manager_init`, `register_http` before the
  server starts, `start` after `http_server_manager_start`.
- README: the BLE bench role + the flashing note.

### WiCAN side (`ble_manager`)

- `POST /api/ble/blast {"bytes":N,"chunk":490}` (`ble_manager_http.c`):
  streams N bytes of a counting pattern through `ble_manager_send()` on
  the data pipe from a task on a PSRAM stack, with the reference's
  pipelining (retry on ENOMEM, no drop); answers immediately with
  `{"started":true}`; `GET /api/ble` gains `blast:{running,bytes,ms}`.
  Reachable over the tunnel, so it works with WiFi handed over.
- No change to DLE handling: the peripheral answers the central's DLE
  request (the July "do not force DLE" note is about the peripheral
  forcing it under coex; with handover the radio is clean).
- `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` 12 -> 24 and
  `TRANSPORT_EVT_SIZE` 70 -> 255 only if the bench shows ENOMEM retries
  or dropped events (measure first; PSRAM-backed).

### Bench

`tools/testbench/ble/ble_sim_central_bench.py` (PC, drives the simulator
over USB-NCM `http://192.168.8.1/api/ble_bench` and reads the WiCAN over
the hotspot before/after the link): connect -> `notify` 60 s -> `write`
60 s -> `read` 10 s -> `tunnel_up`/`tunnel_down` 64 KB and 512 KB ->
disconnect, on `phy=1m` then `2m`; `METRIC` lines; verdict
`BLE THROUGHPUT PASS` = notify >= 300 kbps and write >= 400 kbps at 1M
with WiFi handed over (the reference minus margin), every tunnel
transfer byte-exact, 0 stack errors; 2M reported. `test.ps1 blethru`,
benchboard, TESTING.md row.

## Order

1. Component skeleton + settings + HTTP/CLI + pure tunnel codec with host
   tests (no hardware).
2. NimBLE central: scan/connect/tune/pair/discover; `notify`/`write`/`read`.
3. WiCAN `POST /api/ble/blast`; build + flash the WiCAN.
4. Simulator wiring, sdkconfig, build. **Ali flashes the simulator.**
5. Bench script, runs at 1M and 2M, thresholds, docs, TESTING.md.
6. (Ali's decision) stream-channel OUT direction on notifications with
   credits, measured with the same instrument.

## Status (2026-09-21 night): first radio runs

Flashed twice (Ali holds BOOT; the CH342 on COM116 has no EN/IO0 lines, so
the supply on COM50 is cycled to boot the new image). Run 3 of
`ble_sim_central_bench.py`, WiCAN on `phy=1m`, WiFi handed over:

| leg | 1M | 2M setting on the central (link stayed 1M: the WiCAN's `1m` pins it) |
|---|---|---|
| connect / pair / discover | one attempt, MTU 517, interval 7.5 ms, DLE 251/251, bonded | same |
| notify 60 s (blast 4 MB) | **479 kbps**, 7554 PDUs, 0 errors | 489 kbps |
| write 60 s (write-without-response) | 252 kbps, 3702 writes, 56 894 ENOMEM retries | 299 kbps, 57 109 retries |
| read 10 s (DIS 2A29, 11 B) | 636 reads = 15.7 ms per round trip | 647 reads |
| tunnel up/down | stalled: credit timeout at 16 384 B, then 429 busy | same |

Findings:
1. **Notify beats the reference** (479 vs ~340 kbps) on the very first run;
   the WiCAN's data pipe + `POST /api/ble/blast` is the July path and it
   holds under the throughput_app tuning.
2. **The tunnel stall was the central's fault**: it sliced writes at
   MTU-3 = 514 B, but the WiCAN caps every attribute payload at 490 B
   (`ble_manager_gatt_svc.c` answers INSUFFICIENT_RES above that, which a
   write-without-response never shows) and NimBLE's attribute limit is 512.
   Every body write was dropped silently, the device kept waiting for the
   body, and answered the next requests 429 until its 30 s idle timeout.
   Fixed: `bcb_payload_cap()` = min(490, MTU-3) for every write. The same
   cap applied to the write leg, so its 252-299 kbps are for writes the
   device REFUSED; the real figure comes with the fix.
3. The write leg's 57 k ENOMEM retries per minute say the central's TX
   path is buffer-bound (MSYS 50 blocks, each 514 B write = 2 mbufs + the
   LL fragments in flight at 7.5 ms). Watch this after the cap fix; the
   WiCAN's own RX buffers (MSYS 12+24, ACL 24) are the next lever.
4. `phy=2m` on the central alone does nothing against a WiCAN on `1m`
   (by design since the PHY work); the bench now sets the DUT to `auto`
   for the run and restores it.
5. The WiCAN logged **one unexpected reset** across the run (boot_count 2,
   unexpected_resets 1); the 16 KB ring had rolled over before it could be
   read and a console-attached repro of the 64 KB tunnel upload did not
   reproduce it. Open: run the full bench with the console attached.
6. After each BLE session the WiCAN's STA fell back to the dongle AP, so
   the DUT reads go through its own AP when the hotspot lease is gone
   (`dut_api()` fallback).

## Status (2026-09-21 evening)

- Component written (8 files, all under 700 lines, 0 `ESP_LOGE`), host suite
  7/7 on rpi001, compiled into the simulator firmware (`wican-ecu-sim.bin`
  1,513,600 B, 64 % of the 4 MB app partition free) with the NimBLE central
  sdkconfig block applied.
- WiCAN side: `POST /api/ble/blast` built, flashed on the bench unit and
  answering (409 `no BLE link` without a central).
- Bench driver + `test.ps1 blethru` + benchboard + TESTING.md row written.
- **Waiting for the simulator's first flash (Ali: BOOT held through a
  reset, then `build_flash.ps1 -port COMx`).** Nothing has run on the
  radio yet; expect a debug loop on the first connect (discovery chain,
  passkey injection, the WiCAN's connection-update request).

## Results, run 4 (2026-09-21, corrected central, WiCAN console attached)

WiCAN on `phy=1m` (the bench's DUT-`auto` step failed on ssh quoting, so
both legs ran on 1M; the "2M" column is the central's preference only),
`sta_ble_handover` on, 7.5 ms interval, MTU 517, DLE 251/251, one connect
and pairing per leg, **no reset** (the "unexpected reset" of run 3 was the
supply cycle that booted the simulator: a power-on reset counts as
unexpected).

| leg | 1M | 2nd leg (also 1M) | esp-idf reference |
|---|---|---|---|
| notify 60 s (blast 4 MB, FFF1) | **478 kbps** | 463 kbps | ~340 kbps |
| write 60 s (490 B write-without-response to FFF2) | 345 kbps, 5281 writes, 56 522 ENOMEM retries | 352 kbps | ~500 kbps |
| read 10 s (DIS 2A29, 11 B) | 649 reads = 15.4 ms each | 640 | ~200 kbps @ 510 B |
| tunnel_up 64 KB / 512 KB (`POST /api/fs/upload`) | **558 / 488 kbps** | 517 / 472 kbps | (app level) |
| tunnel_down 64 KB / 512 KB (indications, byte-exact) | **233 / 223 kbps** | 233 / stalled at 448 KB | (app level) |

Readings:
- **Notify matches and beats the reference.**
- **The app-level tunnel is now real**: 60 KB/s up, 28 KB/s down, byte-exact,
  vs 2.2-5.7 KB/s down measured with the Pi and PC centrals earlier today.
  The whole difference is the connection interval (7.5 ms here vs 45 ms
  from BlueZ/Windows): one 490 B indication per two intervals = 15 ms.
  Downloads are still indication-paced (233 kbps is the ceiling at 7.5 ms);
  notifications + credits would lift that toward the 478 kbps notify figure.
- **Write is 345 kbps, not 500**, with ~56 k ENOMEM retries per minute on the
  central: the link is buffer-bound. Both boards are short of internal RAM
  under this load (next point).
- **The WiCAN runs with ~8 KB of free internal heap** (`WICAN MEM
  internal_free=7991 internal_largest=7680` at every boot today, BLE on)
  and the console shows `E (…) BLE_INIT: Malloc failed` x3 at the second
  connect and once during the 512 KB download of the second leg, followed
  by `httpd_txrx: error in send: 11` (the loopback socket) and the channel's
  `tx credit timeout` 30 s later: the stall at 448 KB. The controller's
  runtime allocations are internal-RAM only. This is the WiCAN's real
  ceiling for BLE throughput and a standard-§10 E line; candidates: the
  `wifi_ram_profile=lean` setting (frees ~14 KB), MSYS/ACL counts, and a
  look at what holds internal RAM (`WICAN RAMMAP wifi_manager=46672`).
- The blast task's 2 ms retry produced 936 `tx queue full` W lines per leg
  in the IO layer; the retry pause is 8 ms now.
- `POST /api/ota/upload` works on the simulator (running `ota_0`, next
  `ota_1`), so no more BOOT presses.

## Results, runs 5 and 6 (2026-09-21 night): the answer

Same setup, blast pacing 8 ms, WiCAN on `phy=auto` for run 6, no BLE
stop/start between the two links of run 6. Every radio leg PASSED on both
PHYs; every tunnel transfer byte-exact.

| leg | 1M (run 5 / run 6) | **2M (run 6)** | esp-idf reference (1M) |
|---|---|---|---|
| notify 60 s, FFF1 | 485 / 488 kbps | **967 kbps** (4 MB in 34.7 s) | ~340 kbps |
| write 60 s, 490 B write-without-response | 420 / 420 kbps | **974 kbps** | ~500 kbps |
| read 10 s, DIS 2A29 (11 B) | 644 / 647 reads = 15.5 ms each | 637 reads | ~200 kbps @ 510 B (not comparable) |
| tunnel up 64 KB / 512 KB | 548 / 486, 554 / 488 kbps | **969 / 827 kbps** (121 / 103 KB/s) | (app level) |
| tunnel down 64 KB / 512 KB (indications) | 236 / 221, 236 / 222 kbps | 241 / 225 kbps | (app level) |
| 2M / 1M | | notify 1.98x, write 2.32x, read 1.00x | |

So: **the WiCAN matches and exceeds the Espressif throughput_app on 1M
(notify 488 vs 340, write 420 vs 500 with the 400 threshold met) and
doubles it on 2M (967 / 974 kbps).** The app-level tunnel uploads at
60-120 KB/s. Downloads sit at 28-30 KB/s on either PHY because the OUT
direction is one 490 B indication per ATT round trip (2 x 7.5 ms): the
PHY cannot help there, only notifications with app-side credits can (the
notify leg shows the ceiling: 60 KB/s at 1M, 121 KB/s at 2M).

Findings that stay open:
1. **BLE stop/start on the WiCAN degrades the controller's internal
   memory.** Runs 4 and 5: the bench's DUT read through the WiCAN's own AP
   between the two links stops BLE (a station on the AP) and restarts it;
   right after the restart `E BLE_INIT: Malloc failed` x3, and the second
   link's 512 KB download died at 448 KB / 308 KB with one more
   `Malloc failed`, `httpd_txrx: error in send : 11` on the loopback socket
   and the channel's 30 s `tx credit timeout`. Run 6 without the restart:
   the same download passed. `WICAN MEM internal_free=7991
   internal_largest=7680` at every boot today (BLE on): ~8 KB of free
   internal heap is the WiCAN's ceiling, and a BLE restart eats into it.
   Candidates: `wifi_ram_profile=lean`, MSYS/ACL counts, what the
   controller frees on `nimble_port_deinit`. A user hits this whenever a
   phone joins the WiCAN's AP while BLE is up.
2. The write leg's ~56 k ENOMEM retries per minute on the central are the
   central's own TX pool; harmless (the reference does the same yield).
3. Bench plumbing: the WiCAN's STA falls back to the client-isolated dongle
   AP after every reset, so the DUT-side settings steps (phy auto/restore)
   need the AP path with retries; they failed in runs 5/6 while every
   radio leg passed. Run the driver from the Pi or park the dongle AP for
   BLE benches.
4. Simulator: OTA in place (`build_flash.ps1 -ota`), no BOOT button needed
   any more.

## Results, run 7 (2026-09-22 00:04): notify mode downloads (ble_http v2)

Ali's decision after the run-6 report: move the tunnel's OUT direction to
notifications with app-side credits. Implemented as ble_http **protocol v2**
(4-bit body-frame counter in `flags[7:4]`, CREDIT both ways, one frame per
notification PDU, 16 KB device-side window) on a stream channel whose OUT
mode the central picks with its CCCD (`ble_manager` `out_modes`); the
simulator client and the Pi client updated; host suites ble_http 25,
ble_manager 10, ble_central_bench 9 all PASS. Same rig, `phy=auto` on the
WiCAN for the run, 7.5 ms, MTU 517, DLE 251.

| leg | 1M | 2M | run 6 (indications) |
|---|---|---|---|
| notify 60 s | 485 kbps | 966 kbps | 488 / 967 |
| write 60 s | 417 kbps | 969 kbps | 420 / 974 |
| tunnel up 64 KB / 512 KB | 558 / 485 kbps | 1008 / 900 kbps | 548-554 / 486-488, 969 / 827 |
| **tunnel down 64 KB / 512 KB, notifications** | **482 / 483 kbps** (60 KB/s) | **931 / 951 kbps** (119 KB/s) | 236 / 221, 241 / 225 |
| tunnel down 512 KB, indications (same firmware) | 223 kbps | 222 kbps | |
| notify / indicate download | 2.17x | 4.28x | |

Downloads now run at the radio's rate on both PHYs (the goal). **But 2 of
the 4 notify downloads had one hole**: 1M 64 KB arrived as 65054 B (one
482 B frame missing, counter 7 -> 9), 2M 512 KB as 523806 B (one frame).
The counter caught both deterministically (`exact=False holes=1`), which is
what v2 is for; the bench grades them FAIL on purpose.

### Where the PDU goes (the arbiter run, Pi + btmon, same night)

`ble_http_probe.py` reworked to need no WiFi (everything through the
tunnel, `GET /api/ble` before/after each download for the device's PDU
count; btmon on the UB500 for the on-air count; the client's count from the
AcquireNotify socket). 6 + 8 downloads of 64 KB in notify mode, 45-60 KB/s:

```
run 0: LOSS 64572/65536 B holes 1 | device tx 141 PDUs | air 142 PDUs | client 142 PDUs   (2 PDUs short)
run 2: LOSS 63608/65536 B holes 1 | device tx 141 PDUs | air 140 PDUs | client 140 PDUs   (4 PDUs short)
run 1,3,4,5: OK                    | device tx 141      | air 144      | client 144        (144 = 141 + the 3 PDUs of the second /api/ble read)
```

air == client in every run: BlueZ (AcquireNotify) loses nothing. The
missing PDUs are consecutive (one hole of 1-4 frames) and never reach the
receiving controller; the WiCAN's host had counted them as sent. The same
loss showed on two different receivers (ESP32 NimBLE central, Realtek
UB500), and the reverse direction (simulator -> WiCAN write-without-response
at 969 kbps, 512 KB byte-exact, `400 hole` never fired) is clean, so the
sender side of the WiCAN is the place.

Ruled out on the way: gating each notification on
`esp_vhci_host_check_send_available()` (NimBLE's ESP glue only logs when it
is false and sends anyway; Bluedroid waits on it) changed nothing (8 runs:
2 LOSS of 4 and 1 PDU). What differs between the WiCAN and the simulator
as senders: the WiCAN runs WiFi coex and NimBLE mbuf pools of 12 x 256 +
24 x 320 blocks (the simulator has 50); `ble_hs_wakeup_tx_conn()` frees a
queued packet silently on any error but EAGAIN.

### The fix (2026-09-22, three experiments, same probe, 64 KB notify downloads)

| firmware change | lossy runs | lost PDUs |
|---|---|---|
| none (run 7 firmware) | 2 of 6, then 2 of 8 | 2+4, then 4+1 |
| + gate every notification on `esp_vhci_host_check_send_available()` | 2 of 8 | 4, 1 |
| + NimBLE mbuf pools 12/24 -> 32/48 blocks (PSRAM) | 3 of 10 | 1, 1, 1 |
| **+ `CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=12`** | **0 of 12** (1728 PDUs) | **0** |

The ESP32-S3 controller (libbtdm) allocates each ACL TX buffer from the
INTERNAL heap at transmit time when the static count is 0 (Kconfig help:
"dynamically allocating: allocate before TX and free after TX"). The WiCAN
boots with ~8 KB of internal heap free; a failed allocation inside the
controller drops the PDU with nothing for the host to see, hence PDUs
counted by NimBLE that never reached the air, on any receiver. 12 static
buffers (the Kconfig maximum) cost ~3 KB of internal RAM at controller init,
which the dynamic path was consuming under load anyway. The same starved
heap is behind the `E BLE_INIT: Malloc failed` lines after a BLE
stop/start (run 4/5 finding) - still open. The VHCI gate and the bigger
pools are kept (harmless; the pools live in PSRAM).

### Run 8 (2026-09-22, static ACL buffers): the answer

| leg | 1M | 2M |
|---|---|---|
| notify / write 60 s | 479 / 389 kbps (write re-run alone x3: **490** each) | 966 / 956 kbps |
| tunnel up 64 KB / 512 KB | 554 / 480 kbps | 1038 / 863 kbps |
| **tunnel down notify 64 KB / 512 KB** | **477 / 466 kbps, exact, 0 holes** | **961 / 971 kbps, exact, 0 holes** |
| tunnel down indicate 512 KB | 214 kbps | 225 kbps |

Every radio and tunnel leg PASS on both PHYs; the verdict line carried the
one-off 389 (490 x3 on re-run) and the DUT-settings restore step (dongle-AP
plumbing, as in runs 5-7). Final build (after the `ble_manager_gatt_tx.c`
split) re-verified with the probe: 12 of 12 downloads clean, 1747 PDUs,
host = air = client. Downloads went from 28-30 KB/s (indications, any PHY)
to 60 KB/s on 1M and 120 KB/s on 2M, with every lost PDU detectable.

### What the fix costs, and what the console showed afterwards (2026-09-22 01:00-02:30)

- 12 static ACL TX buffers = **3.2 KB internal RAM** at controller init:
  `WICAN MEM internal_free` at boot with BLE on went 7991 -> 4787.
- With `wifi_ram_profile = full` the bench DUT then ran into the cliff within
  minutes of a WiFi + BLE session: `E httpd: Failed to post esp_http_server
  event: ESP_ERR_TIMEOUT` every 2 s (the default event loop could not take
  the web server's events), the tunnel's loopback requests failing
  (`ble_http: loopback response head failed (-28679)` -> 502 to the app),
  `system -m`: RAM Free 5447 B, largest block 3072 B, **Min free ever 155 B**.
  This is the same starved heap as the `BLE_INIT: Malloc failed` item of
  runs 4/5 and the reason BLE did not come back after an AP station left.
- `wifi_manager.wifi_ram_profile = lean` (its README: "use lean to run WiFi
  + BLE together") -> boot free **18403 B** (+13.6 KB), `system -m` after
  15 min of WiFi + BLE + tunnel: 16.5 KB free, min 5.1 KB. **The bench DUT
  runs lean from now on**; the setting change is the one deliberate change
  to Ali's DUT configuration this session (phy=1m and handover=on restored).
- Also seen on the console, unrelated to BLE and left alone: the console
  `system -r` reboot hung in `data_logger` ("stop: the writer did not park
  within 3 s", then `param ring full` for minutes, `dl_writer` at 22% CPU),
  `ha_webhooks` POSTs failing every 15 s (no internet on the dongle AP),
  WireGuard handshakes failing. And the rig: the Pi's UB500 dongle went deaf
  (raw `hcitool lescan` sees nothing while the PC sees 21 advertisers); it
  needs a physical re-plug.
- PC client (`ble_http_probe_pc.py`, bleak/WinRT): 14 downloads byte-exact,
  0 holes, but WinRT subscribes for INDICATIONS and refuses a CCCD write,
  so the PC path runs at 5.6 KB/s (45 ms interval). A native app picks.
