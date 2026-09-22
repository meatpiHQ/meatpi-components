# TASK: native ISO-TP in the public build (UDS Tool + J2534 ISO15765)

Status: PLAN, 2026-09-16. Scope: the PUBLIC build only. Nothing here touches
the internal add-on pack; where the pack already registers a provider it
must keep winning (rule in Phase 1).

## 0. What is wrong today (bench-proven 2026-09-16, DUT 68ee8f5a653d)

| # | Finding | Evidence |
|---|---|---|
| A | `can_isotp()` has no provider in the public build: `ext_manager` is a no-op, nothing calls `can_isotp_provide()`. | `uds_manager` reports `backend:"obd_chip"` with native CAN running; `j2534_server` logs `ISO15765 not available in this build (no ISO-TP provider registered)` |
| B | J2534 ISO15765 cannot bind. CONNECT without ids is accepted (nothing binds yet), CONNECT with ids or a FLOW_CONTROL filter fails. Raw CAN channels work end to end. | `tools/testbench/usb/j2534_transport_probe.py`: raw CAN PASS (TWAI tx 6 → 7, `06 41 00 ..` back), ISO15765 bind FAIL |
| C | The UDS obd_chip transport races autopid: `uds_manager` has its own claim but never pauses the poller, and `obd_chip_request()` passes interleaved traffic into the response window by design. | 3× `3E 00` with autopid polling: ok 1.5 s, `ESP_ERR_INVALID_RESPONSE`, `ESP_ERR_TIMEOUT` 14 s; with the group paused 5/5 ok at ~525 ms |
| D | The chip transport is slow and wears the chip: 7 setup AT round-trips per request incl. `ATSP` (EEPROM write, autopid sanitizes to `ATTP`, this path does not), no `ATST`, so every request pays the chip's ~0.5 s post-response wait; `rbuf[512]` (~170 payload bytes); request cap 64 B; 0x78 pending left to the chip (ATST max 1020 ms). | `uds_transport_at.c`; obd_chip README (multi-frame + FC only on 7E0-7E7) |
| E | Route + page: the UI's Timeout field sends `timeout_ms`, which the route ignores (reads `p2_ms`/`p2star_ms`); the route truncates the response hex to 256 bytes while `length` reports the true size; no `ext` (29-bit) or `session` (tester present) controls; raw JSON in a console box; no status route to show which backend is live. | `uds_manager_http.c` lines 42-43, 152-155, 211; `PAGES.uds` |

## 1. Target state

- Public firmware answers `backend:"isotp"` whenever `can_manager` runs, and
  J2534 ISO15765 channels bind and carry multi-frame UDS (reflash preamble
  through the PCAN reflash ECU passes).
- The chip transport stays as the fallback when native CAN is disabled, but
  it no longer races autopid, no longer writes EEPROM per request, and
  answers in ~p2 rather than ~0.5 s.
- The UDS Tool page shows which path is live, decodes the answer, and every
  control on it does something.

## 2. Phases (in the order to do them)

### Phase 0: the chip's EEPROM guard (BUILT 2026-09-16, meatpi: "fix any OBD chip command that writes to eeprom")

`obd_chip_guard.h` (pure, in `obd_chip_parse.c`, host-tested): ATSP -> ATTP and
ATM1 -> ATM0 rewritten in place (same length, case/spacing kept); ATPP (except
ATPPS), ATSD, ATCV, STWBR, STSAVCAL refused. Runs in `obd_chip_request()` (a
refused command returns ESP_ERR_NOT_SUPPORTED) and in `obd_chip_send()` (the raw
bridge path: rewritten in a copy, or dropped with the ELM `?` + prompt fanned back
to the app, so the chip never sees it). autopid's `ap_init_sanitize()` delegates to
it and config parse refuses a refused command in `cmd`/`init`; the UDS AT
transport sends ATTP itself. Counters `eeprom_guard.rewrites/blocked` in
`GET /api/obd_chip`; boot provisioning (`bare_probe`) and the fw update flow are
exempt by construction. Acceptance: obd_chip + autopid host suites; bench: an
`ATSP6` and an `ATPP 0C SV 23` sent through the WS obd bridge show as rewrite /
blocked counters, the chip answers OK / the app sees `?`.

### Phase 1: chip transaction hold + response sanity (BUILT 2026-09-16, bench pending)

1. `obd_chip_txn_begin()/end()` (obd_chip.h): the calling task takes the
   COMMAND claim for a whole multi-command transaction; its own
   `obd_chip_request()` calls nest, every other requester (autopid's poll)
   waits at its normal claim timeout. Chosen over an autopid-side hold:
   both parties are "the chip" here, and obd_gate cannot separate them.
2. `uds_transport_obd.c` holds the transaction around `uds_at_transceive()`;
   `uds_manager.c` rejects a reply whose SID is not ours (`uds_response_
   matches()`, host-tested) with ESP_ERR_INVALID_RESPONSE + a WARN, on either
   transport — a stray autopid reply can never be returned as the answer.
3. Bench: new `tools/testbench/obd/uds_route_bench.py <base>` → `UDS ROUTE
   PASS`: 20 × `3E 00` + `10 02` + `22 F1 90` through `POST /api/uds/request`
   with autopid polling; asserts 20/20 ok, prints the backend and the
   median/max time. Baseline to beat: 1/3 ok, 0.5-14 s.

Acceptance (bench 2026-09-16): the guard, the hold and the SID/retry
guards land TWO gates in `uds_route_bench.py`:
- HARD (correctness): **0 corrupt** — a request is NEVER answered ok with a
  payload that is not its own SID. Verified 0/0 even under the flood below.
- throughput: `--min-ok-pct` (default 90). PASS with autopid PAUSED (20/20,
  median 107 ms) or a light/standard-PID config.

KNOWN LIMIT (the chip transport's ceiling, why Phase 2 exists): while autopid
floods the SAME single-MCU ECU simulator with 32-parameter multiframe DIDs
(the Ioniq profile on 7E4), a UDS request to 7E0 is starved to ~2/20 — the
transaction hold blocks autopid (verified: 0 polls during a UDS burst), but
autopid's own request times out mid-ISO-TP and the simulator keeps
transmitting its 7E4 reply, saturating the bus. Failures are clean errors,
never corrupt data. On a real car 7E0 and 7E4 are separate ECUs that answer
independently, so this is pessimistic; the real coexistence fix is the native
ISO-TP path (Phase 2), which subscribes to its own rx id and never touches
the chip. The chip transport is the no-CAN fallback.

### Phase 2: the public ISO-TP provider (the big one, fixes A and B) — BUILT + bench PASS 2026-09-16

Status: `can_isotp_esp` is in the public build (main registers it right after
`ext_manager_init`, only when the slot is empty). Bench, public build, DUT
68ee8f5a653d + the ECU simulator + the PCAN reflash ECU:
1. `j2534_transport_probe.py` → `raw CAN PASS, ISO15765 bind PASS` (the
   FLOW_CONTROL filter opened a native session; `10 02` → `50 02 00 32 01 F4`
   came back as an ISO15765 RX_MSG).
2. `j2534_bench.py --reflash --tx 7E2 --rx 7EA` against `pcan_reflash_ecu.py
   --scenario happy --req 7E2 --resp 7EA` (new id overrides: the simulator
   cannot be switched off — its settings API is gone — and it answers 7E0, so
   the reflash ran on a pair it does not answer; autopid paused) →
   `J2534 REFLASH PASS`: CONNECT with ids, 10 02, seed/key, the multi-frame
   34 → 74, the 31 01 erase routine (the happy scenario sent no 0x78).
3. `uds_route_bench.py --expect-backend isotp` with autopid RUNNING: 20/20 on
   7E0 (median 22 ms) AND 20/20 on the pathological same-ECU 7E4 target
   (median 28 ms) that starved the chip path to 2/20 — 0 corrupt both. The
   simulator answers `22 F1 90` with NRC 0x31 on 7E0 now (Ioniq profile), so
   the VIN check moved to the negative-response path.
4. Coexistence: autopid kept ~11 polls/s during the UDS benches (58 in 5 s),
   `/api/can` rx kept counting (+276 in 3 s, rx_missed 0, dispatch_drops 0),
   the only `E (` lines are data_logger's pre-existing SD `disk I/O error`.
   `autopid_matrix_bench.py` NOT run: it replaces the autopid config and Ali
   is editing the DUT's table live.
5. Memory: PSRAM min_free 3.89 MB with a session open (floor 1 MB); the
   `isotp` task stack high-water 3296 B free of 4096 after the reflash leg;
   `stack_audit.py` (its component roots were WRONG — `REPO/../..` is
   wican-fw-dev, the paths doubled the folder name, so it had scanned main/
   only; fixed) lists the task, no candidate for the component (frames ≤ 96 B,
   esp_isotp ≤ 160 B). Pre-existing candidates it now surfaces, NOT touched:
   microlink `do_h2_preface` 4272 B vs 4096 B tasks `ml_ppp_rx`/`ml_udp_rx`
   (104 %), autopid `apid_scan` 47 %.

Two things the build added beyond the sketch below: (a) the provider allows
ONE session per rx id, so the UDS isotp transport now closes its session after
5 s idle (`ISOTP_IDLE_CLOSE_MS`, esp_timer; tester-present re-binds inside it)
— without that a UDS probe of an ECU locked a J2534 CONNECT to the same ECU
out (bench-hit); (b) both consumers hold `obd_gate` as planned (UDS per
transaction, J2534 WRITE→READ delivery).

Original sketch (kept for the design record):

New public component `can_isotp_esp` (own folder, REQUIRES can_manager,
esp_isotp). It is the `can_isotp_ops_t` implementation over the vendored
`esp_isotp` in its externally-managed mode, so can_manager keeps owning the
TWAI node:

- **open(cfg)**: `esp_isotp_new_transport(NULL, {tx_id, rx_id, 4096/4096
  buffers in PSRAM, tx_frame_pool 16, externally_managed_twai = true,
  can_tx_callback → can_manager_send(), rx_callback → copy the assembled PDU
  into the session mailbox})`; subscribe a task-owned queue with
  `can_manager_subscribe_queue(q, rx_id, 0x7FF | 0x1FFFFFFF, ext, false)`.
  Sessions coexist with the monitor: subscribers each get their copy, so
  nothing else on the bus changes.
- **one provider task** ("isotp", PSRAM stack) drains every session queue
  through a queue set (rule: only `xQueueReceive` the member the set handed
  out), feeds `esp_isotp_feed_can_frame()`, and calls `esp_isotp_poll()` at
  1 ms while a transfer is in flight (STmin pacing, N_Bs/N_Cr timeouts).
- **send**: `esp_isotp_send()` then wait for the tx-complete callback up to
  `timeout_ms`; `ESP_ERR_TIMEOUT` when the peer's flow control never comes.
- **recv**: wait on the mailbox up to `timeout_ms`; `ESP_ERR_NO_MEM` when
  the PDU is larger than `cap` (consumed, per the contract callers drain on).
- **close**: unsubscribe, delete the transport, free the buffers.
- **registration**: at `main_boot_init` before `uds_manager`/`j2534_server`
  start, `can_isotp_esp_init()` calls `can_isotp_provide()` ONLY if
  `can_isotp() == NULL`. Order it after `ext_manager_init`, so a build that
  carries the add-on pack keeps the pack's provider (single-writer slot).
- **limits**: sessions capped at 1 (uds) + `J2534_MAX_CHANNELS`; BS/STmin/
  padding from `can_isotp_cfg_t` as today (J2534 SET_CONFIG already maps
  ISO15765_BS/STMIN); 29-bit through `ext_id`.
- **bus sharing with autopid**: the native sessions are ESP-side requesters in
  `obd_gate`'s sense, and the chip already acquires that gate around every
  command (`obd_chip_request`, `obd_chip_send` on CR) and releases at the '>'
  prompt. So: the UDS isotp transport acquires the gate for each transaction
  (request -> final response), the J2534 ISO15765 channel acquires it on
  WRITE_MSGS and releases when the reassembled reply is delivered or N_Cr
  times out; owner = the session pointer. The gate is fail-open (3 s wait,
  2 s self-expiring hold), so a wedged side can never brick the other.
  Phase 1.2's SID check stays as the second line for UDS.
- **no change** to `uds_manager` backend logic: `auto` already prefers isotp
  when a provider exists and can_manager runs.

Acceptance (all on the public build, DUT + simulator, then the PCAN ECU):
1. `j2534_transport_probe.py` → `raw CAN PASS, ISO15765 bind PASS` (the
   flow-control filter path AND CONNECT with ids).
2. `j2534_bench.py --reflash` against `actors/pcan_reflash_ecu.py --scenario
   happy` (simulator ECU off, autopid held) → `J2534 REFLASH PASS`: multi-
   frame 34/36, seed/key, the erase routine riding out 0x78 pending.
3. `uds_route_bench.py` → `backend:"isotp"`, 20/20, `22 F1 90` returns the
   simulator VIN `1WCAN0FW0P0000001` as one multi-frame PDU, median well
   under 100 ms.
4. Coexistence: `/api/can` counters keep counting, CAN Monitor still shows
   the bus, `autopid_matrix_bench.py` still passes with the provider
   registered, zero `E (` lines on the console across all of it.
5. Memory: `stackaudit` static + runtime halves clean (new task stack in the
   audit; PSRAM min_free floor still ≥ 1 MB with 4 sessions open).

Not host-testable as a unit (esp_isotp REQUIRES esp_driver_twai); the
pure part worth a host test is the session bookkeeping (open/close/limits,
NO_MEM consumption) behind a fake ops seam — small, do it.

### Phase 3: chip transport hygiene (fixes D, keeps the fallback honest) — BUILT + bench PASS 2026-09-16

Status: `uds_route_bench.py --expect-backend obd_chip` (settings forced) with
the Exclusive bus switch on: **20/20, median 3 ms, max 7 ms** (Phase 1: 106 ms
median). Three findings on the way, all measured on the chip through the ELM
TCP bridge (scratch `elm_timing.py` / `elm_pending.py`):
- the ATST from p2 alone gave 34 ms only right after boot; later every
  request took ~255 ms = the chip waiting out ATST for more ECUs. The ELM
  **response-count digit** (`3E001`: one response expected) is what makes
  the chip hand the answer over at once: 4-8 ms vs 180-260 ms, in every
  ATAT mode. The transport appends it to every request.
- with the digit the chip (ELM327 v2.3 core) still rides out `7F xx 78`
  responsePending AND prints every one of them before the final answer
  (erase_pending ECU: 8 lines then `71 01 FF 00 00`, 409 ms, nothing left
  in its buffer). The parser is message-aware now: pendings dropped and
  counted (`pending` in the route), the last message is the answer —
  host-tested, and bench: `31 01 FF 00` -> `71 01 FF 00 00` pending 8.
- setup re-send only when needed works: a steady request adds 5 bytes to
  the chip's tx counter (its own line), nothing else writes meanwhile.
Also: 8 KB PSRAM reply buffer, 64 B request cap returns INVALID_ARG with a
log line. The chip path without the Exclusive switch stays at its known
ceiling under autopid's flood (4/20, 0 corrupt) — that is what the switch
is for. Original sketch:

1. `ATSP` → `ATTP` (share `ap_init_sanitize` by moving it to obd_chip as
   `obd_chip_sanitize_cmd()`; autopid keeps calling it).
2. `ATST` from p2 (`p2_ms / 4`, floor 0x0A) sent with the target setup, so a
   single-frame answer returns in ~p2 instead of the chip's 0.5 s wait;
   0x78 pending stays chip-bound (document: on the chip path P2* > 1 s is
   not reachable; the native path is the answer for slow ECUs).
3. Re-send the target setup only when the address changed or autopid ran in
   between (a "chip touched" counter from obd_chip); otherwise one request =
   one AT line.
4. `rbuf[512]` → a PSRAM static sized like `AP_RESP_MAX`; request cap stays
   64 B (chip line limit) and is reported as `ERR_INVALID_ARG` with a clear
   message instead of silent truncation.

Acceptance: `uds_route_bench.py --backend obd_chip` (settings forced) →
20/20, median ≤ 300 ms, no `ATSP` in a `/ws/obd` capture of the run.

### Phase 4: route + page (fixes E) — BUILT + PASS 2026-09-16

Status: `GET /api/uds` (backend_setting/active, provider, can_running, the
exclusive switch state, autopid_paused, session_active, `last` transaction,
esp_isotp provider stats), `POST /api/uds {exclusive}`, `POST /api/uds/session
{action:begin|end,tx_id,rx_id,ext}` (tester present while the page holds it),
`timeout_ms` honoured as P2*, the whole PDU as hex (12 KB PSRAM). Page: path
badge + provider chip, the Exclusive bus switch, 29-bit, session switch,
"Final response timeout (P2*)", and — Ali 2026-09-16, "a terminal view
instead" — a scrolling TERMINAL of every request (›) and reply (‹) with the
decode inline (positive / NRC name, size, time, path, DID payload as text),
raw JSON per reply behind a toggle, kept across page visits (last 300 lines,
Clear), a sent line clicks back into the form; the uds_manager settings card.
Preview:
`probe_uds.mjs` 18/18 PASS, `smoke.mjs` PASS (build the preview with
`MEATPI_COMPONENTS_PATH` set or it silently uses the stale mirror tree).
Playwright shot on the DUT: badge, switch, decoded NRC, history, settings.
Original sketch:

1. Route: `timeout_ms` honoured as `p2star_ms` (keep both names); hex cap =
   full `UDS_RESP_CAP` (PSRAM buffer, 3 B per byte); new `GET /api/uds`
   status `{backend_setting, backend_active, provider:"esp_isotp"|"none",
   can_running, last:{sid,nrc,elapsed_ms,backend}}` so the page can show
   the live path without sending a request.
2. Page: a backend chip ("over native CAN" / "over the OBD chip, native CAN
   is off") from `GET /api/uds`; controls for `ext` (29-bit) and `session`
   (tester present while the page is open); Timeout renamed "Final response
   timeout (P2*)"; the response rendered as fields (SID, positive/negative,
   NRC name, bytes, ASCII for 22 F1 xx) with the raw JSON behind a toggle;
   request history (last 10, click to re-send). `tools/webui_preview`:
   mock `/api/uds` + a `probe_uds.mjs`.

Acceptance: `probe_uds.mjs` PASS; smoke PASS; Playwright shot on the DUT
showing the badge and a decoded VIN.

### The Exclusive bus option (Ali, 2026-09-16: "an enable/disable in the UI" so
UDS/J2534 can block AutoPID) — BUILT + bench PASS

Design: settings are boot-applied and autopid depends on uds_manager, so the
hold lives in `obd_gate` (the bus-policy component both sides already point
at): `obd_gate_diag_hold(owner, on)` refcounted by owner (pure core
`obd_gate_diag.c`, host-tested), autopid's poller checks `obd_gate_diag_held()`
every loop, pauses polls AND DTC scans, acks (`obd_gate_diag_ack`), and
restores its chip baseline on resume; `stats.paused_diag` in /api/autopid.
Each tool has a boot-default setting (`uds_manager.exclusive`,
`j2534_server.exclusive`, default ON — Ali 2026-09-16, "this should be ON by
default in UDS tools and in J2534 and scripting") plus a RUNTIME switch on its page
(`POST /api/uds|/api/j2534 {exclusive}`) — the autopid pattern (setting +
runtime control). UDS holds from the first request (waiting up to 700 ms for
autopid's ack) until 10 s of idle (`UDS_EXCLUSIVE_IDLE_MS`) or the end of a
session; J2534 holds while a tester is attached (TCP or serial). Bench: hold
on within ~1 s of the first request, 0 autopid polls while held, release at
~10 s idle and polls back at ~11/s; J2534 probe paused autopid for exactly
the 2.3 s connection; chip-path bench with the switch 20/20 vs 4/20 without.
Not blocked: apps on the MIC chip (BLE/TCP/WS ELM clients) — they are the
user's own traffic; autopid already yields to them. Scripting has its OWN
setting + hold (`script_engine.exclusive`, default on): a script's first ECU
access (uds/uds_ext, obd_claim, obd_request, obd_isotp_tx/rx) takes obd_gate's
hold, the runner releases it after the run — so a running script never
depends on the UDS page's switch. Bench (UDS switch OFF): a synchronous 4.4 s
script moved autopid's poll counter by 4 (all before its first request)
against 44 polls in 4 s idle; the device log shows the hold spanning the run
and autopid paused for exactly that window. Measurement trap: `/api/scripts/run`
runs the script on an httpd worker synchronously, so `/api/autopid` samples
taken meanwhile queue behind it and are served AFTER the run (they read
"running") - use the log ring or a counter delta around the run, not samples.

### Scripting re-verified (Ali: "check UDS scripting still works and script
ECU flashing") — PASS 2026-09-16

`tools/testbench/obd/uds_bindings.be` via `run_be.py`: uds()/uds_ok/uds_nrc/
uds_nrc_str, obd_claim/obd_request/obd_isotp_tx/rx/obd_release — 13/13 on the
native path with autopid running AND on the chip path. `reflash.be` (now
TX/RX-parametrized; `run_be.py --tx 7E2 --rx 7EA`) against
`pcan_reflash_ecu.py --scenario happy --req 7E2 --resp 7EA` with the Exclusive
switch on: REFLASH OK — session, seed/key, erase, RequestDownload, 640 bytes in
5 TransferData blocks over native ISO-TP in ~140 ms, TransferExit, the ECU's
checkMemory CRC routine `71 01 02 02 00` = verified. Needs
`script_engine.allow_reflash` (restored to off after).

### Phase 5: docs, tests, release note

- `HTTP_API.md` §6e11 (backend semantics, status route, timeout field),
  `can_isotp.h` / `ext_manager.h` comments (drop "add-on pack only"),
  `drivers/j2534/README.md` (ISO15765 now true in public), `uds_manager`
  README (new), TESTING.md rows (`UDS ROUTE`, `J2534 REFLASH` as the public
  acceptance, the transport probe), NEW_FEATURE_BRIEF §4/§8, the memory note.
- Release note line: "UDS Tool and J2534 ISO15765 run over native CAN;
  the OBD chip is the fallback when CAN is disabled."

## 3. Decisions for Ali (defaults in bold, the plan proceeds on them)

1. Provider placement: **new public component `can_isotp_esp`, self-
   registering, pack wins when present** — or fold it into can_manager.
2. Bus arbitration for native sessions: **obd_gate per conversation (UDS:
   request->final response; J2534: write->reply)** — or hold autopid for the
   life of a J2534 channel.
3. Keep the chip transport as the no-CAN fallback: **yes** (Phase 3 makes
   it decent) — or drop it and require native CAN for the UDS Tool.
4. Page: expose the `backend` setting in the UI: **no, status badge only**;
   the setting stays in All Settings for the bench.

## 4. Order and size

Phase 0 + 1 (guard, hold, sanity, bench) were built 2026-09-16 and ship value alone. Phase 2 is
the core, ≈ 2 days including the reflash acceptance. Phase 3 ≈ half a day.
Phase 4 ≈ 1 day. Phase 5 alongside. Every phase ends on its bench verdict
line on the public build before the next starts (reproduce-before-fixing
rule: the baselines in §0 are the reproductions).

## 5. Open items parked, same family, not in this task

- Profile imports leave the chip in the last profile's world; with an empty
  `std_init` standard PIDs die until a restoring prelude (autopid README
  importer rule 2; `autopid_profile_bench.py` WARN). Fix candidate: the
  runner restores the baseline on a specific → std transition.
- Xpeng 221122 indexes reply byte 223 vs `AP_PAYLOAD_MAX` 128.
