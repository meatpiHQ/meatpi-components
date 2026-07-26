# ble_manager — on-target test app + rpi001 RF bench (standard §7)

The production composition: `ble_manager` registered as a bridge endpoint,
`br_ble` = `ble` ↔ `echo` raw bridge (everything a client writes to FFF2
comes back as FFF1 notifications), and a stand-in CLI handler (upper-cases
the line, answers on CLI OUT) until `cmdline_manager` exists.

The DUT only prints readiness/link markers — the real assertions run from
**rpi001's BLE controller** (Ali's call: the Pi is the BLE test client).

## Tools + versions (external-tools rule)

| Item | Value (verified 2026-07-04) |
|---|---|
| DUT | WiCAN Pro (ESP32-S3), test app on IDF v6.0.2, COM7 console @115200 |
| BLE client | rpi001 (Pi 5 onboard controller), BlueZ 5.66, bleak 3.0.2, python3-dbus (passkey agent) |
| Script | `tools/testbench/ble_bench.py` — run ON the Pi |

## Procedure

1. Flash this app (`.\test.ps1 target ble_manager` builds/flashes; the app
   idles after `BLE READY` so the capture step times out by design — drive
   the Pi side instead). Serial shows:

   ```
   INIT ok=1
   START ble=1 bridge=1
   BLE READY name=WiC_<device id>
   LINK connected=1 / secured=1 / ...   (as the client attaches)
   ```

2. On rpi001 (once: `pip3 install --break-system-packages bleak`,
   `sudo apt install python3-dbus python3-gi`; remove a stale bond with
   `bluetoothctl remove <mac>`):

   ```
   python3 tools/testbench/ble_bench.py --passkey 123456
   ```

## Pass criteria + results (2026-07-04, first flight)

Expected final line: **`BLE BENCH PASS`**. Covered + measured:

- Scan finds `WiC_<id>` advertising FFF0 ✓
- Static-passkey pairing via the D-Bus agent; all data characteristics are
  `ENC_MITM`, so the working reads/notifies prove the encrypted+
  authenticated link ✓
- Device Information byte-identical to legacy (`MEATPI.COM`, `WiCAN-PRO`,
  serial `4c19f44e349` = device name + 7) ✓
- Echo byte-exact through FFF2 → ble_manager → bridge pump → echo endpoint
  → pump → FFF1 ✓
- CLI: `hello cli\n` to CLI IN → `HELLO CLI` on CLI OUT ✓
- Perf (full benchmark: `tools/testbench/ble_perf.py` + `../BENCHMARKS.md`):
  TX notify **60.2 KB/s** (256 KB blast, zero loss), echo **37.9 KB/s each
  way**, RTT steady-state **p50 4.6 ms** / p95 12.7 / p99 35.6 (the first
  seconds after connect run slower until the 20 ms conn interval applies)

The app's CLI also accepts `blast <bytes>` — the TX-throughput trigger the
benchmark uses. Cleanup: reflash main firmware; `bluetoothctl remove <mac>`
on the Pi for a pristine re-pair (a stale client-side bond against a
re-flashed DUT disconnects during service discovery).
