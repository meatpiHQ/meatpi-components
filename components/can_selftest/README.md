# can_selftest

The CAN self-test (feature component, issue #1023, 2026-10-28). The
ESP32-S3's own CAN controller (can_manager, the TWAI transceiver) and the
MIC3624 OBD chip (obd_chip) sit on the same CAN_H and CAN_L wires (the
schematic's CAN block: two transceivers on one pair). The test makes them
talk to each other with one standard OBD exchange: the chip asks for engine
speed the way every OBD app does (`010C` to the functional id 7DF) and the
ESP32 answers it as an engine ECU from **7EF**, ECU number 8's response id,
which no production car uses. One exchange proves the chip's transmit and
the ESP32's receive (the request), the ESP32's transmit and the chip's
receive (the reply), and both acknowledgements, with or without a vehicle:
on a live vehicle the real ECUs answer 7E8.. too and the test looks only for
its own line (Ali, 2026-10-28: a live bus runs, reported as `live`).

## API (see `include/can_selftest.h`)

| Function | What |
|---|---|
| `can_selftest_init/start/stop` | lifecycle (§3): the settings descriptor (`{cli}`) and the log descriptor; nothing runs until a test is asked for |
| `can_selftest_run_start()` | the test on a one-shot task (the HTTP route); `ESP_ERR_INVALID_STATE` while one runs; the preconditions land in the result's `refusal` |
| `can_selftest_run(out)` | the test in the caller's task (the console), about 2 s |
| `can_selftest_last(out)` | the job's result (a copy), running or done |
| `can_selftest_parse_reply(resp, want_id, &rpm)` | pure: the chip's answer classified (`ours` with the rpm decoded, `other`, `no_data`, `can_error`, `error`, `none`) |
| `can_selftest_verdict(esp_heard, reply, &pass)` | pure: the one-line verdict, PASS or where to look |
| `can_selftest_register_http()` / `can_selftest_register_cli()` | `GET/POST /api/can/selftest`; the `can_selftest` command (self-registered on settings apply, §6b) |

## The sequence (about 2 s)

1. Preconditions: the CAN bus running (`can_manager_status`), the chip up
   (`obd_chip_ready`) and idle (`obd_chip_client_idle_ms` over 10 s: an app
   on the chip would read the exchange as its own), the bus readable
   (`can_manager_probe`: silent or live both run; unreadable refuses). A
   listen-only node on a silent `auto` bus adopts the configured bitrate
   (500 when auto) so it may transmit; `can_manager_tx_ready` decides.
2. The ESP32 subscribes a queue to 7DF and reads the TWAI counters.
3. The chip, in one transaction (`obd_chip_txn_begin`; `obd_chip_client_touch`
   makes AutoPID yield): `ATTP6` (500 kbit/s) or `ATTP8` (250), `ATH1`,
   `ATSH 7DF`, `ATCAF1`, then `010C` written raw (`obd_chip_send`) while this
   task plays the ECU: the 7DF frame `[02 01 0C]` arrives on the queue within
   400 ms, the reply `7EF [04 41 0C 1A F8]` goes out (`can_manager_send`),
   and the chip's output is collected through the fan-out to its prompt
   (900 ms).
4. Restore: `ATPC`, `ATD` (the protocol follows the setting again; AutoPID's
   prelude re-inits on resume), the transaction ended, the queue released.
5. The verdict (`can_selftest_parse.c`): PASS when the ESP32 heard the request
   and the chip printed the ESP32's line (engine speed 1726 rpm). Otherwise
   where to look: `CAN ERROR` = nothing acknowledged the chip (the shared
   wires or the termination); the ESP32 heard nothing while other ECUs
   answered = the ESP32's transceiver or standby pin; the ESP32 heard nothing
   and nobody answered = the chip's CAN side or protocol; `NO DATA` after the
   ESP32 replied = the ESP32's transmit path.

Three steps with their ms and the exchange line by line are kept for the
card and the console; a failed test logs W, never E (a user-run diagnostic).

## Surfaces

- `POST /api/can/selftest` -> 202 `{"started":true}` (409 while a run is in
  progress); `GET /api/can/selftest` -> `{running, done, pass, verdict,
  refusal, bus:{baud_kbps, live}, esp_heard, reply, rpm, took_ms,
  steps:[{name, ok, ms, detail}], lines:[]}`. Documented in
  `components/HTTP_API.md` of the app repo.
- `can_selftest` (the last result) and `can_selftest run`.
- The web UI: the CAN self-test card on System > Maintenance (web_ui_v2
  `web/diag.js`).

## Settings (`"can_selftest"`, version 1)

| Key | Type | Default | Notes |
|---|---|---|---|
| `cli` | bool | `true` | the `can_selftest` console command (§6b) |

## Dependencies

can_manager, obd_chip (both must be init'd and started by main first),
settings_manager, log_manager, cmdline_manager, http_server_manager.

## Memory footprint (estimated)

| Where | What | Size |
|---|---|---|
| PSRAM `.bss` | the job's result and the console's copy (2 x ~1.6 KB), the chip output buffer (512 B), the HTTP copy (~1.6 KB) | ~5.5 KB |
| Internal heap, transient | the job task's stack (4 KB, UART and TWAI driver paths), the two queues (8 CAN frames + 32 chip chunks, ~4.5 KB) during a run | ~8.5 KB for 2 s |
| PSRAM heap, transient | the GET's body (4 KB) | 4 KB per request |

## Tests

- Host (`host_test/`, 9 tests): the reply classifier on the chip's real
  line forms (headers on, with and without spaces, 29-bit ids, a reply from
  another ECU only, `NO DATA`, `CAN ERROR`, `?`, empty), the rpm decode,
  the verdict table.
- Bench: `tools/testbench/can/can_selftest_bench.py` on rpi001 =
  `.\test.ps1 canselftest` in the app repo (simulator broadcast off = PASS
  on a silent bus; broadcast on = PASS with `live:true`; CAN disabled and
  an app on the chip = the refusals), TESTING.md row.
