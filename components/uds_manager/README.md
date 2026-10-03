# uds_manager

UDS (ISO 14229) request and response over a selectable transport: the MIC3624
OBD chip (`obd_chip`, always there) or firmware ISO-TP on the native CAN bus
(`can_isotp_esp` over `can_manager`). The component owns the protocol: the
`7F xx 78` responsePending loop, the negative-response decode, the
tester-present keepalive of a session, one transaction at a time; the
transport is a thin vtable, so both backends get the protocol for free.

Consumers: the UDS Tool of the web UI (`POST /api/uds/request`, sessions),
the `uds` console command, the `uds.request` action of the event manager,
the Berry `script_engine` (`uds()` and the raw `obd_isotp_tx / _rx`
bindings), and autopid's DTC engine (the UDS and WWH-OBD paths of a scan
and a clear ride `uds_request()`; the pure codec of this component builds
their requests and parses their answers).

First README 2026-10-03 (TASK_j1939_wwh.md phase 7); the component dates
from 2026-09.

## Files

| File | What |
|---|---|
| `include/uds_manager.h` | The API: `uds_request()`, sessions, raw ISO-TP, status, the exclusive option |
| `include/uds_proto.h`, `uds_proto.c` | PURE. Negative response codes and their ISO names, the responsePending shape |
| `include/uds_dtc.h`, `uds_dtc_codec.c` | PURE. Service 19 / 14 builders and parsers: classic (`19 01 / 02 / 0A`, `14 <group>`), WWH-OBD (`19 42`, `19 55`, `14 FF FF 33`: 5- and 4-byte records, DTC formats 04 = J2012 "P0420-08" and 02 = J1939-73 "SPN3226-4") |
| `include/uds_transport.h` | The transport vtable (one op: a request to its FINAL response) and the pure AT-hex helpers |
| `uds_transport_obd.c`, `uds_transport_at.c` | The chip transport: protocol (`ATTP6/7` at 500 kbit/s, `ATTP8/9` at 250, RAM-only), header, receive filter, headers off + auto-formatting so the chip does ISO-TP; the setup is skipped in a steady conversation (the chip's tx byte counter says whether autopid re-addressed it since) |
| `uds_transport_isotp.c` | The native transport: a raw UDS PDU over the registered ISO-TP provider on `can_manager`'s bus |
| `uds_manager.c` | The protocol, the claim, the session, the exclusive option (obd_gate's diagnostics hold), status |
| `uds_manager_settings.c` | Settings descriptor |
| `uds_manager_http.c` | `GET /api/uds`, `POST /api/uds` (the runtime exclusive switch), `POST /api/uds/request`, `POST /api/uds/session` |
| `uds_manager_cli.c` | Console command `uds` |
| `uds_manager_events.c` | The `uds.request` action and the `uds.response` source of the event manager |
| `host_test/` | Host suite of the pure files (31 tests): NRC names, the AT-hex parser, the DTC codec incl. WWH-OBD |

## API

| Function | What |
|---|---|
| `uds_manager_init()` / `_start()` / `_stop()` | Lifecycle (main's init / start / sleep passes) |
| `uds_request(&addr, req, n, resp, cap, &len, &opts, &result)` | One transaction to its final response: the 0x78 loop (`p2star_ms` each, `max_pending` 20), the outcome decoded (`negative`, `nrc`, `nrc_name`, `pending_count`, `elapsed_ms`, `backend`). A negative response is a completed transaction (`ESP_OK`, `result.negative`); `ESP_ERR_TIMEOUT`; `ESP_ERR_INVALID_STATE` while another transaction or a session holds the transport, or the transport's bus is down |
| `uds_session_begin(&addr)` / `uds_session_end()` | A tester-present session (`3E 80` every `tester_present_ms`) that holds the claim so a whole exchange stays on one ECU |
| `uds_isotp_tx()` / `uds_isotp_rx()` | Raw ISO-TP PDUs, no UDS semantics (the isotp backend only; `ESP_ERR_NOT_SUPPORTED` otherwise): the scripts' `obd_isotp_tx / _rx` |
| `uds_manager_get_status(&st)` | The snapshot of `GET /api/uds` |
| `uds_manager_set_exclusive(on)` / `uds_manager_exclusive()` | The runtime exclusive switch |
| `uds_manager_active_backend()`, `uds_manager_backend_name()` | Which transport serves right now |
| `uds_at_set_can_kbps(kbps)` (uds_transport.h) | The bit rate the chip transport pins its protocol to (`ATTP8/9` at 250, `ATTP6/7` at 500). The chip transport sets it itself before a target setup from `can_manager`'s last probe (the native controller's listen-only look at the bus; a fresh one is taken when the last is older than a moment): a live bus names its rate, a silent one leaves 500, a bus that reads at neither rate refuses the request (phase 2 / 3) |

`uds_addr_t` = `tx_id` (tester to ECU), `rx_id` (ECU to tester), `ext_id`
(29-bit). The chip's ISO-TP covers 11-bit `7E0..7E7` answered on `7E8..7EF`
and 29-bit `18DAxxF1` / `18DAF1xx`; its reply buffer is 512 bytes (a longer
answer needs the native transport). Timing settings: `p2_ms` (first
response, 250), `p2star_ms` (after each 0x78, 5000), `tester_present_ms`
(2000).

**Backend `auto`**: the native ISO-TP when `can_manager` runs in normal mode
with an ISO-TP provider registered, the chip otherwise. The public builds
ship no provider (the add-on jack's), so `auto` is the chip there.

**The exclusive option** (`exclusive`, default on; the runtime switch on the
UDS page / `POST /api/uds {"exclusive":bool}`): while the tool is in use (a
request, then `UDS_EXCLUSIVE_IDLE_MS` = 10 s of idle, or an open session) the
background pollers stay off the bus through obd_gate's diagnostics hold:
autopid's PID polling and DTC scans, and since phase 6 the requests of its
J1939 `?` rows (`/api/autopid` shows `stats.paused_diag`). The hold waits
for the poller's acknowledgment (it loops within 500 ms) before the first
request goes out, so it lands on a quiet bus. `script_engine` and
`j2534_server` hold the same gate for their sessions.

## The DTC codec (pure, host-tested)

Classic (ISO 14229): `uds_dtc_req_count` (`19 01 <mask>`),
`uds_dtc_req_by_status` (`19 02 <mask>`), `uds_dtc_req_supported` (`19 0A`),
`uds_dtc_req_clear` (`14 <group>`), `uds_dtc_parse_count`,
`uds_dtc_parse_list` (4-byte records), `uds_dtc_clear_ok`,
`uds_dtc_format` / `_unformat` ("P0420", "P2463-1F" when the failure type
byte is not 0).

WWH-OBD (ISO 27145-3 / SAE J1979-2; phase 3): `uds_wwh_req_by_mask`
(`19 42 <group> <status mask> <severity mask>`; the legislated testers send
status `08` confirmed / `04` pending and severity `1E`, the four DTC
classes), `uds_wwh_req_permanent` (`19 55 <group>`), `uds_wwh_req_clear`
(`14 FF FF <group>`: all of the group or nothing), `uds_wwh_parse_by_mask`
(`59 42 group statusAvail severityAvail format (severity dtc dtc dtc
status)*`), `uds_wwh_parse_permanent` (`59 55 group statusAvail format (dtc
dtc dtc status)*`), `uds_wwh_dtc_text` (format 02 = "SPN<spn>-<fmi>", every
other format the J2012 text). The emissions group is `0x33`
(`UDS_WWH_FGID_EMISSIONS`). autopid's `autopid_dtc_wwh.c` asks each
discovered responder physically through `uds_request()` with these.

## Settings

Component `uds_manager`, reboot-to-apply (the exclusive switch also applies
at once at runtime).

| Key | Default | What |
|---|---|---|
| `backend` | `auto` | `auto` / `obd_chip` / `isotp` |
| `p2_ms` | 250 | First-response timeout, 50..5000 |
| `p2star_ms` | 5000 | Timeout after each 0x78, 500..30000 |
| `tester_present_ms` | 2000 | Session keepalive period, 500..10000 |
| `exclusive` | `true` | The boot default of the exclusive bus option |
| `cli` | `true` | Register the `uds` console command |

## HTTP (`components/HTTP_API.md` has the shapes)

`GET /api/uds` (backend setting and active backend, provider, the exclusive
state and whether the pollers acknowledged the hold, the session, the last
transaction), `POST /api/uds {"exclusive":bool}`, `POST /api/uds/request
{"tx_id","rx_id","ext"?,"data":"22F190","p2_ms"?,"p2star_ms"?,"timeout_ms"?
(the page's final-response timeout = P2*),"session"?}` (the final response,
the NRC decoded, the pending count, the elapsed time; 409 while another
transaction or a session owns the bus), `POST /api/uds/session
{"action":"begin","tx_id","rx_id","ext"?}` / `{"action":"end"}`.

## Console

`uds -t <txid> -r <rxid> [-e] <hex request>`, e.g. `uds -t 7E0 -r 7E8 22 F1
90`: the response in hex, or the negative response decoded.

## Events

Action `uds.request` (`tx`, `rx`, `req` hex, `ext`; blocking: it runs off
the dispatcher). Source `uds.response` (`ok`, `nrc`, `len`, `data`
truncated, `req`); a script's `uds()` binding has the full payload.

## Dependencies

`PRIV_REQUIRES obd_chip can_manager can_isotp_esp event_manager obd_gate
settings_manager log_manager esp_timer http_server_manager esp_http_server
espressif__cjson cmdline_manager console`. Init after `obd_chip` and
`can_manager`; start after `can_manager_start()` (the `auto` backend looks
at it); stop before `can_manager_stop()`.

## Limits worth knowing (bench, 2026-09 to 2026-10)

- The chip is single-master: a transaction races autopid's polling for the
  MIC (5 of 5 answered with autopid paused, 1 of 3 while polling before the
  exclusive option existed); the exclusive hold is the answer, not a retry.
- One chip transaction costs about 7 AT round trips plus `ATSP` when the
  setup is not skipped; the chip's ISO-TP only covers `7E0..7E7`; its reply
  buffer is 512 bytes.
- A request pinned to a 500 kbit/s protocol on a live 250 kbit/s bus
  destroys that bus's traffic (phase 2): the chip transport asks the native
  controller's probe before every target setup and pins protocols 8 / 9 on
  a 250 kbit/s bus; a bus that reads at neither rate gets no request at all
  (one `W` line, `ESP_ERR_INVALID_STATE`).

## Tests

- Host: `.\test.ps1 host uds_manager`, 31 tests (NRC names, the AT-hex
  parser incl. the 29-bit header print shape, the classic and WWH-OBD DTC
  codec in both DTC formats).
- Bench: the UDS legs of `tools/testbench/obd/uds_route_bench.py` (`UDS ROUTE
  PASS`) and `uds_dtc_bench_test.py` (`UDS DTC BENCH PASS`); the WWH-OBD
  path through `.\test.ps1 wwh` (`WWH OBD PASS`: `19 42` in both DTC
  formats, `19 55`, the clear gate, `7F 78` pending frames); the exclusive
  hold is exercised by the J1939 active bench (a J2534 tester attached).
