# ble_http

> App developers: the wire contract is [`BLE_HTTP_PROTOCOL.md`](BLE_HTTP_PROTOCOL.md);
> the GATT surface it rides on is `ble_manager/BLE_API.md`.

## Summary

The HTTP API over BLE. A `ble_manager` stream channel (`http`, FFF3
notify / FFF4 write under service 0xFFF0) carries framed HTTP requests
from a paired phone; the device replays each one against its own web
server over loopback (`http://127.0.0.1:<port>`) with `esp_http_client`
and streams the response back. No per-route code: every `/api/*` route
in `components/HTTP_API.md` works, so storage management (`/api/fs/*`),
the UDS terminal, settings, status, logs, AutoPID, scripts and OTA are
available to an app that never joins WiFi. Requested by Ali 2026-09-21
("users should be able to develop their own apps over BLE").

Why loopback: `api_http` and the 30 other route files bind handlers to
`httpd_req_t` directly; there is no route table a second transport could
re-drive. Replaying through the server keeps one implementation, one
document and one behaviour (incl. the network-trust gate, which lets
loopback through). The handlers run on the httpd task's INTERNAL stack,
so the `/api/fs/*` flash work never runs on this component's PSRAM task.

## API

| Function | Purpose |
|---|---|
| `ble_http_init()` | settings ("ble_http") + log descriptors |
| `ble_http_start()` | registers the channel with ble_manager (when enabled) and starts the tunnel task. MUST run before `ble_manager_start()` |
| `ble_http_stop()` | stops the task (sleep path); the channel stays registered |
| `ble_http_status(out)` | enabled / registered / link / active request / counters (`ble_http_status_t`) |
| `ble_http_register_cli()` | `blehttp`, self-registered from `on_apply` |

Pure core (`ble_http_core.h`, host-tested): `bleh_hdr_parse/build`,
`bleh_req_parse`, `bleh_rsp_head`, `bleh_core_init/rx/tick/link_down`
behind an injected `bleh_ops_t` (open/write/fetch/read/close) and an emit
callback.

## Dependencies

`ble_manager` (stream channel), `http_server_manager` (the loopback
port), `esp_http_client`, `settings_manager`, `log_manager`,
`cmdline_manager`, cJSON.

## Settings (`ble_http`, version 1, reboot-to-apply)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | register the channel. BLE itself is opt-in (`ble_manager.enabled`, default false) and the channel is served only on a paired MITM-authenticated link, so this does not widen the trust surface; off = the characteristics are absent, zero cost |
| `cli` | `true` | register the `blehttp` console command |

## Memory footprint (estimated, 2026-09-21; measure with `WICAN MEM`)

| Where | What | Size |
|---|---|---|
| PSRAM `.bss` | channel RX StreamBuffer storage (2 x the 16 KB credit window) | 32 KB |
| PSRAM `.bss` | frame reassembly + TX staging (2 x 4104 B), read slice 512 B | 8.7 KB |
| PSRAM `.bss` | task stack | 8 KB |
| internal | TCB, `StaticStreamBuffer_t` (in ble_manager's registry) | ~0.4 KB |
| heap (per request) | `esp_http_client` handle + 2 x 1 KB buffers, freed after the response | ~4 KB |
| flash | | ~7 KB |

No flash writes from this component. The task must stay on PSRAM: it
never touches the filesystem itself.

**Known limitation (2026-09-21):** `esp_http_server` serves one request
at a time, and a tunnelled transfer runs INSIDE the route handler at BLE
speed, so a long BLE upload/download holds the web server for WiFi
clients meanwhile (a 512 KB transfer is 4-9 s on a 7.5 ms link since v2,
minutes on a 45 ms indication link). Short requests are unaffected. Fix
candidate: stage the request body / response in PSRAM (heap, sized per
request) so the loopback exchange runs at memory speed; until then the
docs tell app developers.

**Protocol v2 (2026-09-22): notify mode.** The channel registers with
`out_modes = INDICATE | NOTIFY`; the app's CCCD on FFF3 selects. In
notify mode the core streams the response from `bleh_core_pump()` (called
by the task between 1 ms channel reads, so the app's `CREDIT` / `ABORT`
frames are taken while a download runs), one frame per PDU
(`min(490, MTU-3) - 8` bytes), at most `BLEH_OUT_WINDOW` 16 KB
unacknowledged, and every `REQ_BODY` / `RSP_BODY` frame carries a 4-bit
counter (`flags[7:4]`) so a PDU lost inside a stack is a detected hole
(`400 hole` upstream, the client's `holes` counter downstream) instead
of a silent desync. Indications keep 4096 B frames and need no credits.
The counter earned its place on day one: the first notify downloads lost
one PDU in about 500 (bench run 7, holes flagged deterministically), and
the btmon arbiter showed the WiCAN's host had counted PDUs the air never
saw. Root cause: the controller's heap-allocated ACL TX buffers
(`CONFIG_BT_CTRL_BLE_STATIC_ACL_TX_BUF_NB=0`) on a device with ~8 KB free
internal heap; `=12` fixed it (0 holes in 12 + the whole bench). A phone
stack can still drop, so the counter stays.

## Testing

- Host: `host_test/` (25 Unity cases: frame codec, request/response
  heads, the FSM against a fake HTTP side: credits, short/overrun,
  busy, abort, idle timeout, link down, chunked, upstream failures;
  v2: notify-mode PDU-aligned frames, the 16 KB OUT window waiting for
  app CREDITs, indicate mode needing none, `REQ_BODY` counter holes,
  credit timeout truncation, ABORT/busy while responding, MTU 23 frames).
  On rpi001 copy `managed_components/` + `dependencies.lock` from
  `event_manager/host_test` first (offline from the registry).
- Bench: `tools/testbench/ble/ble_http_pi.py` on rpi001 (via
  `ble_pi_run.py` = `test.ps1 blehttp`) -> `BLE API PASS` with a nested
  `BLE STORAGE PASS`: status / info / settings round trip, storage legs
  on `/data` and `/sd` (1 KB / 64 KB / 512 KB byte-exact, SHA-256
  cross-checked over WiFi, device tx == on-air == client rx), error
  cases, disconnect mid-upload, UDS request/session against the ECU
  simulator, a chunked text body. **2026-09-21: PASS**; measured on the
  bench link (UB500 central, WiFi STA up beside BLE, MTU 517): uploads
  1.9-2.1 KB/s, downloads 2.2-2.6 KB/s, `GET /api/status` ~1 s round
  trip. `TESTING.md` row `blehttp`. `ble_http_probe.py` is the
  focused download probe (N runs with btmon + `GET /api/ble` counters:
  device tx vs on-air vs client rx) that located the notification drop
  behind the indication decision.

## Files

`ble_http.c` (glue: channel, task, esp_http_client vtable),
`ble_http_core.c/.h` (pure), `ble_http_settings.c`, `ble_http_cli.c`,
`include/ble_http.h`, `BLE_HTTP_PROTOCOL.md`, `host_test/`.
