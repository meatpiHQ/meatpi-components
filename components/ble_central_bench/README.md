# ble_central_bench

A NimBLE **central** bench instrument: connects to one WiCAN (`WiC_*`),
tunes the link the way esp-idf's `examples/bluetooth/nimble/throughput_app`
does (preferred MTU 517, DLE 251 B / 2120 us, connection interval 7.5 ms,
PHY preference), pairs with the fixed passkey (KeyboardOnly, injected), and
runs **timed GATT throughput tests** against the device's own
characteristics. The measurement side of the BLE app platform
(`ble_manager/BLE_API.md`); first host is the ECU simulator firmware
(`wican-ecu-sim-fw`). Plan + results: `TASK_ble_central_bench.md`.

Reference figures (throughput_app README, 1M PHY, MTU 512, 7.5 ms, DLE):
NOTIFY ~340 kbps, WRITE ~500 kbps, READ ~200 kbps.

## API (`include/ble_central_bench.h`)

| Function | Contract |
|---|---|
| `ble_central_bench_init()` | settings + log descriptor + CLI registration hook; before `settings_manager_start` |
| `ble_central_bench_register_http()` | `/api/ble_bench` GET/POST; before the HTTP server starts |
| `ble_central_bench_start()` / `_stop()` | brings the NimBLE controller + host up iff `enabled`; the bench task (8 KB PSRAM stack) |
| `ble_central_bench_connect()` / `_disconnect()` | queued to the bench task: scan for `target`, connect, tune, pair, discover FFF0 (FFF1..FFF4 + CCCDs) and DIS 2A29; `ESP_ERR_INVALID_STATE` unless idle |
| `ble_central_bench_run(mode, seconds, size)` | queued: `notify` (subscribe FFF1, ask the peer to blast `size` bytes through the tunnel's `POST /api/ble/blast`, count), `write` (write-without-response to FFF2 for `seconds`), `read` (DIS 2A29 loop), `tunnel_up` / `tunnel_down` (`size` bytes to/from `/data/blebench/blob.bin` over the `http` stream channel, byte-exact check) |
| `ble_central_bench_get_status()` | state, peer link parameters (MTU, interval, PHY, DLE), handles, last result (`bytes`, `ms`, `kbps`, `count`, `errors`, `retries`, `http_status`, `exact`, `detail`), counters |

## Settings (`ble_central_bench` v1, reboot-to-apply)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | false | a bench role: opt-in |
| `target` | `WiC_` | advertised-name prefix (or a full name) to connect to |
| `passkey` | 123456 | the peer's fixed passkey |
| `mtu` | 517 | preferred ATT MTU |
| `conn_itvl_units` | 6 | connection interval in 1.25 ms units (6 = 7.5 ms); also what we answer the peer's update request with |
| `ce_len_units` | 24 | max connection event length, 0.625 ms units (min = half) |
| `dle` | true | request 251-byte LL packets right after connect |
| `phy` | `1m` | `1m` / `2m`: controller default preference + a per-link request |
| `cli` | true | `blebench` console command |

## HTTP (`/api/ble_bench`)

GET: `{"enabled","state":"off|idle|scanning|connecting|connected|running","peer":{"name","addr","mtu","itvl_ms","phy_tx","phy_rx","dle_tx","dle_rx","secured","bonded","rssi"},"chars":{"fff1","fff2","fff3","fff4","dis"},"last":{"mode","running","ok","bytes","count","ms","kbps","errors","retries","http_status","exact","detail"},"counters":{...}}`.
POST: `{"action":"connect"}`, `{"action":"disconnect"}`,
`{"action":"run","mode":"notify|write|read|tunnel_up|tunnel_down","seconds":10,"size":65536,"out":"notify|indicate"}`
(`out` = how FFF3 is subscribed for the tunnel modes, default `notify`:
the client then pays `CREDIT` frames every 4 KB and checks the v2 frame
counter; the result carries `holes`, `credits`, `out`); answers
`{"ok","err"}` (409 when the state refuses).

## Dependencies

`bt` (NimBLE, central + observer roles), `settings_manager`, `log_manager`,
`cmdline_manager`, `console`, `http_server_manager`, `esp_http_server`,
`espressif__cjson`, `esp_timer`, `nvs_flash` (bond store). Builds to an
empty component without `CONFIG_BT_NIMBLE_ENABLED`.

sdkconfig the host firmware needs (the reference's values):
`CONFIG_BT_NIMBLE_ROLE_CENTRAL=y`, `..._OBSERVER=y`, peripheral/broadcaster
off, `CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y`,
`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517`,
`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=50`,
`CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=20`,
`CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE=255`, `CONFIG_BT_NIMBLE_NVS_PERSIST=y`.

## Memory footprint

Bench task 8 KB stack (PSRAM), 32 KB FFF3 reassembly StreamBuffer
(PSRAM) + its control block (internal), a 4 KB frame staging buffer and a
512 B slice (static, `.bss` in PSRAM when the host allows it), NimBLE
itself per its config (MEM_ALLOC_MODE_EXTERNAL recommended). Measured
numbers: TASK doc.

## Testing

- Host: `host_test/` (Unity, `idf.py --preview set-target linux`): the
  pure HTTP-over-BLE client codec (9 tests: header round trip/rejects
  incl. a v1 device, request head JSON, response head parse, feed with
  arbitrary splits, stray/resync, upload credit window, v2 body counter
  holes + wrap, download credits due/frame).
- Bench: `wican-fw/tools/testbench/ble/ble_sim_central_bench.py` drives
  the simulator over `http://192.168.8.1/api/ble_bench` (`test.ps1
  blethru`, verdict `BLE THROUGHPUT PASS`). Results and the notification
  loss investigation (runs 4-8, 2026-09-21/22): `TASK_ble_central_bench.md`.
  The client's `holes` counter is what located the WiCAN controller's
  silent ACL TX buffer drop.

## Files

`ble_central_bench.c` (lifecycle, state, the bench task, the tunnel client
runner, the modes), `ble_central_bench_gap.c` (NimBLE central: scan,
connect, tune, pair, discover, blocking GATT primitives),
`ble_central_bench_tunnel.c/.h` (pure client codec + feed FSM),
`_settings.c`, `_http.c`, `_cli.c`, `include/ble_central_bench.h`,
`ble_central_bench_private.h`.
