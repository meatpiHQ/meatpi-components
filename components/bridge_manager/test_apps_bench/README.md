# bridge_manager — Wi-Fi bench app (the flagship data-path composition)

The FULL production path in one firmware: `wifi_manager` (STA to the bench
AP) + `socket_manager` + `bridge_manager` + `obd_chip`, with endpoint glue
in the composition root (the way `main` will do it) and two bridges:

- `br_obd`:  `obd`  ↔ `obd0`  (TCP:35000) — the real OBD chip on the LAN
- `br_echo`: `echo` ↔ `echo0` (TCP:3334)  — RF benchmark loop

## Tools + versions (standard §7 external-tools rule)

| Item | Value (verified 2026-07-03) |
|---|---|
| DUT | WiCAN Pro (ESP32-S3), test app on IDF v6.0.2, COM7 console @115200 |
| AP | rpi001 (Pi 5) NetworkManager hotspot on wlan1 (RTL8822CU): `sudo nmcli device wifi hotspot ifname wlan1 con-name wican-bench ssid WICAN_TEST_AP password <bench-psk>` (PSK per `components/TESTBENCH.md`, not published) |
| OBD bench | MIC3624 chip + ECU simulator @500k/11-bit (see `components/obd_chip/test_apps/BENCH.md`) |
| Load gen | `tools/testbench/socket_bench.py` (run FROM rpi001 — the 10.42.0.0/24 hotspot subnet is only reachable there) |

## Procedure

1. AP up on rpi001 (command above).
2. Build + flash this app (`idf.py build`, esptool `@flash_args` on
   COM7); capture serial until `BENCH READY ip=<dut-ip>`.
3. From rpi001: TCP to `<dut-ip>:35000`, send `ATI\r` / `VTVERS\r` /
   `0100\r` / `0902\r` — expect ELM327 v2.3, MIC3624 Vx.y.z, a live `41 00…`
   reply and the multi-frame VIN, each ending with `>`.
4. Benchmarks: `python3 socket_bench.py --host <dut-ip> --port 3334
   --scenario latency|tcp_throughput|multiclient`. Record results + RSSI
   (`sudo iw dev wlan1 station dump`) in `../BENCHMARKS.md`.
5. Robustness: `nmcli connection down wican-bench`, wait ~8 s, `up`; poll a
   TCP echo until it works — records the recovery time.
6. Cleanup: `nmcli connection down wican-bench`; reflash main firmware.

## Pass criteria + results (2026-07-03, RSSI −47 dBm)

- OBD leg: all four commands answered correctly through the full chain ✓
- Echo RTT p50/p95/p99: **3.4 / 14.2 / 21.3 ms** (n=772)
- Echo throughput: **224 KB/s each direction simultaneously** (every byte
  crosses the pump twice — conservative full-chain number)
- 4-client fan-out: 46.7 KB/s in / ≈187 KB/s out (echo × 4 clients)
- Wi-Fi drop → **15 s** to full end-to-end recovery ✓
