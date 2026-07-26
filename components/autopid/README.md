# autopid

Scheduled OBD PID polling: requests PIDs through `obd_chip`, parses the
ELM response into payload bytes, evaluates every parameter expression
(`expression_parser`) against that ONE response, and keeps the latest
values in a PSRAM cache that HTTP/CLI/(future) event_manager read.

Design + phased goal checklist: [TASK_autopid.md](TASK_autopid.md).
DTC check/report/clear (sub-module, bench-PASSED 2026-07-08, disabled by
default): [TASK_dtc.md](TASK_dtc.md).
Decisions log there is authoritative; the big ones:

- **The PID is the scheduling unit** (legacy fix #1): a PID with ten
  parameters is requested ONCE per period and that single response
  updates all ten. The scheduler (`autopid_sched.c`) has no parameter
  concept, so the legacy behavior is unrepresentable.
- **No transports inside**: autopid produces values/events; delivery
  (MQTT/HTTP/HA/ABRP) belongs to event_manager and future components.
- **Tables in a file, knobs in settings**: `/data/autopid/config.json`
  (groups/pids/filters/parameters, validated + applied LIVE via
  `PUT /api/autopid/config`) vs the `autopid` settings component
  (enable, per-type enables, init strings, pause voltage, min event
  interval — reboot-to-apply like every component).

## Files

| File | Role |
|---|---|
| `autopid.c` | settings fields, poller task (PSRAM stack, notify-driven), battery-pause watch, core query surface |
| `autopid_runner.c` | the chip-facing poll: type/per-PID init transitions, ATCRA rxheader, request → parse → guard → eval → cache |
| `autopid_filter.c` | the ATMA filter window: MONITOR claim, header/CRA choreography, frame capture (`ap_filter_frame`, pure), retried stop |
| `autopid_std.c` | standard PIDs: vendored SAE table (obd2_standard_pids.h), PURE bit_start→expression mapping + bitmap parser, the async support scan (`/data/autopid/std_scan.json`) |
| `autopid_sched.c` | PURE scheduler: due times, group inheritance/override, fail backoff (×4 after 3), period-0 high-fidelity round-robin, stagger |
| `autopid_resp.c` | PURE ELM text → payload bytes (headers on/off, ISO-TP single/multi, lowest-responder rule, noise/error lines) + the cross-talk guard (`ap_payload_matches_cmd` — a second chip master's response can't be cached as ours) |
| `autopid_config.c` | PURE JSON parse/validate + target file load/save (atomic) |
| `autopid_cache.c` | latest-value slots (PSRAM, mutex), legacy snapshot + detail JSON, plus the **external-value table** (12 name-keyed injected samples: GPS from the ESPNetLink dongle) merged into every snapshot/get/detail |
| `autopid_dtc_codec.c` | PURE DTC codec: 2-byte code ⇄ "P0420", 43/47/4A payload parse, 41-01 MIL/count, clear-condition matrix (`always/if_any/if_only` — mode 04 is all-or-nothing, so "clear specific" = a condition on the present set) |
| `autopid_dtc.c` | DTC engine: async scan job (01-01 gate + 03/07/0A), conditional mode-04 clear (fresh 03 → condition → 04 → confirming 03), periodic due-check, RAM report + diff (events fire per NEW code) |
| `autopid_dtc_db_codec.c` | PURE DTC-database importer: sniff + parse CSV/TSV/semicolon/JSON-map/JSON-array/plain text → canonical sorted form ([TASK_dtc_db.md](TASK_dtc_db.md)) |
| `autopid_dtc_db.c` | database store (/data/autopid/dtc_db) + PSRAM cache w/ binary search: upload/list/delete/search/lookup, report `desc` enrichment — lookups never touch flash |
| `autopid_dbc_codec.c` | PURE DBC codec: BO_/SG_ parser + the signal→expression compiler (Intel/Motorola, signed via multiply-subtract, Motorola-aligned spans; host cross-checked vs a reference decoder) — [TASK_dbc.md](TASK_dbc.md) |
| `autopid_dbc.c` | DBC store (/data/autopid/dbc) + PSRAM cache: upload/list/delete, signal browse/search, and the add-to-filters merge (dry-run validate → atomic save → LIVE reload) |
| `autopid_http.c` | `/api/autopid[/data\|/config\|/dtc*]` (see `components/HTTP_API.md` §6e4/§6e4b) |
| `autopid_cli.c` | `autopid [-l] [-d] [--dtc-scan]` console command (§6b self-registration) |

## Settings (`/api/settings/autopid`, reboot-to-apply)

`enabled` (default **false**), `std_enabled`/`custom_enabled`/
`specific_enabled`, `std_init`/`custom_init`/`specific_init`
(';'-separated AT prelude per type), `std_protocol`, `vehicle`,
`pause_below_mv` (0 = never pause; resumes +0.3 V with 5 s hold),
`pause_follow_sleep` (default **true** — legacy `disable_pid_requests`
parity: requests pause below the sleep threshold via the battery
watch; explicit `pause_below_mv` overrides), `pause_mode`
(`all`|`requests_only`), `min_event_interval_ms`
(10–600000), `cli`; DTC (schema v4, ALL off by default): `dtc_enabled`
(**false** — master gate), `dtc_allow_clear` (**false** — second gate
for mode 04 / UDS 0x14), `dtc_scan_period_min` (0 = on-demand only),
`dtc_pending`/`dtc_permanent` (include modes 07/0A), `dtc_init`
(';'-separated chip prep per scan), `dtc_rxheader` (ATCRA filter —
targeted-ECU scans); UDS transport (v4, TASK_dtc.md §12):
`dtc_protocol` (`obd`|`uds`|`auto` — auto = OBD first, UDS fallback),
`dtc_uds_txid`/`dtc_uds_rxid` (default 7E0/7E8), `dtc_uds_ext`
(29-bit ids), `dtc_uds_mask` (0x19 status mask, default 0x08
confirmed).

DTC events/actions/pull-values + rule recipes (scheduled scan+report,
alert-on-new-code, clear-when-detected, scheduled clear) and the Berry
`dtc_scan()`/`dtc_clear()` bindings: [TASK_dtc.md](TASK_dtc.md) §7-9.
DTC works with polling `enabled=false` (scans only need the backend).

**EEPROM guard (ATSP → ATTP, ATM1 → ATM0)**: every user-supplied chip
command — init strings (type inits, per-PID `init`, `dtc_init`) at
send time, PID `cmd`/`init` fields at config parse, and the test-a-PID
one-shot — is sanitized (case-insensitive, whitespace-tolerant —
`ap_init_sanitize`, host-tested): `ATSP` becomes `ATTP` (same protocol
switch, RAM only) and `ATM1` (memory on — makes the chip store every
later protocol change) becomes `ATM0`. Both write the chip's EEPROM,
and PID commands/inits replay on every poll cycle / type transition,
so either would wear it out (legacy semantics). Monitor commands
(`ATMA`/`ATMR`/`ATMT`) and `ATM0` pass through untouched. Write ATSP
in configs/profiles freely; the chip only ever sees ATTP.

## Config file shape (`PUT /api/autopid/config`)

```json
{
  "groups": [{"name": "driving", "enabled": true, "period_ms": 500}],
  "pids": [{
    "name": "engine", "type": "std|custom|specific", "cmd": "010C",
    "init": "ATSH7E0", "rxheader": "7E8", "group": "driving",
    "period_ms": 0,
    "parameters": [{"name": "rpm", "expression": "[B2:B3]/4",
                    "unit": "rpm", "class": "frequency",
                    "min": 0, "max": 16384}]
  }],
  "filters": [{"frame_id": 666, "monitor_ms": 800,
               "parameters": [{"name": "soc", "expression": "B4/2"}]}]
}
```

`period_ms 0` on a PID inherits the group; group `period_ms 0` =
high-fidelity max-rate (entries round-robin as fast as the chip
answers — **~19 req/s total / 53 ms per request measured**, shared
across all period-0 entries; numbers + guidance in
[BENCHMARKS.md](BENCHMARKS.md)). Periods of 1–49 ms are accepted but
flagged (`stats.sub_floor_pids`). A group named `default` always
exists. Expressions index the
payload INCLUDING the service/PID echo (B0 = 0x41 on mode-01) — legacy
profile expressions work unchanged. **Filter expressions index the
frame DATA (B0 = first data byte, no id echo)** — also the legacy
frame-of-reference.

## Filters (ATMA windows)

A scheduled filter entry opens a bounded monitor window: `ATH1` +
`ATCRA<id>`, MONITOR claim, `ATMA`, collect until a frame with the
filter's CAN id arrives (or `monitor_ms` expires — 50..60000, default
1000), SPACE stop, restore receive-all + headers-off, PID inits
replayed. ONE captured frame updates all the filter's parameters. The
window **holds the chip** — other masters' requests fail fast while it
is open, so schedule filters sparsely (period_ms seconds apart, short
windows). Chip "<DATA ERROR"-style markers after the data bytes are
skipped (legacy semantics; bench-observed with injected frames).

**Protocol/width rule (bench-characterized 2026-07-06)**: the ATCRA
hardware filter only matches ids of the ACTIVE protocol's width — a
29-bit filter needs the chip on a 29-bit protocol (ATSP7/9) and an
11-bit filter needs 11-bit (ATSP6/8). Cross-width combinations are
accepted by the chip ("OK") but the window sees NOTHING and the filter
fails cleanly into backoff — set the protocol via the type init /
`std_protocol` to match your filters. (Monitor-all WITHOUT a CRA shows
both widths regardless of protocol; it is the CRA that is
width-bound.) On 29-bit protocols the monitor prints the id as four
byte tokens (`18 DA F1 10 …`) — handled.

**Short frames**: the captured payload is the frame's REAL length
(DLC), and expressions are bounds-checked against it — an expression
past the end (`B6` on a DLC-2 frame) is skipped for that window
(never a garbage read; `null` in the API), while in-range parameters
on the same frame evaluate normally.

## Standard PIDs: scan once, store

`POST /api/autopid/std_scan` (UI button; prompt "ignition ON" first)
walks the SAE support bitmaps against the configured `std_protocol`,
maps hits to the built-in table and stores ready-made config rows —
names, units, classes and **generated expressions** (`[B2:B3]*0.25`
etc.) — at `/data/autopid/std_scan.json`. The UI copies entries from
`GET /api/autopid/std_scan/result` (or the full catalog at
`/api/autopid/std_table`) straight into the config PUT. Never
auto-rescans. Polling pauses during the scan and resumes with its
init state replayed.

## External values (GPS, and future non-PID samples)

`autopid_publish_external(name, unit, value)` injects a named sample
that is NOT a polled OBD parameter into the same live cache. It joins
`autopid_snapshot()` (→ the HA `autopid_data` push + `${autopid.data}`),
fires the value sink (→ `data_logger`), emits `autopid.param` on change
(→ event rules, `group="external"`), and resolves through
`autopid_get_value()` (→ the dashboard) — so one call reaches everywhere
polled parameters do, with no per-consumer work. The table holds 12
distinct names; a config reload does NOT clear it (external samples are
config-independent).

Today's producer: the ESPNetLink dongle's GPS, published by **main** from
`usb_acm_cli`'s GPS sink as `gps_latitude` / `gps_longitude` /
`gps_altitude` (m) / `gps_speed` (km/h) / `gps_heading` / `gps_satellites`
— only a live fix; last-known persists. autopid itself stays
USB/GPS-agnostic (the glue lives in main, like the data_logger sink).

`autopid_snapshot()` / `GET /api/autopid/data` is a flat `{name: value}`
object — the GPS keys sit right alongside the polled PIDs:

```json
{
  "VehicleSpeed": 62,
  "rpm": 1840,
  "coolant_temp": 89,
  "SOC_BMS": 74.5,
  "gps_latitude": -37.90535,
  "gps_longitude": 145.145047,
  "gps_altitude": 88.8,
  "gps_speed": 61.6,
  "gps_heading": 270.5,
  "gps_satellites": 7
}
```

The raw GPS surface `GET /api/gps` is the contract shape (speed in m/s,
HDOP-derived accuracy in m); `{"valid": false}` when there is no live
fix:

```json
{
  "valid": true, "latitude": -37.90535, "longitude": 145.145047,
  "accuracy": 6, "altitude": 88.8, "speed": 17.1, "heading": 270.5,
  "satellites": 7, "age_ms": 1200
}
```

## Vehicle profiles (client-side import)

The UI/app fetches `vehicle_profiles.json` (GitHub), the user picks a
profile, and the client PUTs the converted table to
`/api/autopid/config` (+ `vehicle`/init strings in settings). The
firmware never fetches profiles. **Two conversion rules the importer
MUST apply** (bench-verified against the published MEB profile):

1. **Byte-index shift**: published profile expressions index the
   legacy frame WITH its PCI byte (`010C` RPM = `[B3:B4]`); the v6
   payload starts at the service echo (B0 = 0x41/0x62) — shift every
   `B<n>`/`S<n>` index DOWN BY ONE (`[B2:B3]`).
2. **Cross-type inits must be self-sufficient**: a specific PID whose
   init switches protocol/headers/CRA (`ATSP7;…;ATCRA…`) leaves the
   chip there — if standard PIDs are polled too, give `std_init` a
   restoring prelude (`ATSP6;ATCRA;ATH0`) so each type transition
   re-establishes its world.

`POST /api/autopid/test` = try a profile PID before saving (same
runner path: init chain, rxheader, expression — see HTTP_API §6e4).

## Tests

`host_test/` (run: `.\test.ps1 host autopid`): scheduler regression
(one-request-per-PID-per-period, group toggle/override, type gates,
round-robin, backoff, stagger), response-parser vectors (single frame,
SEARCHING noise, headers-on lowest responder, ISO-TP both header
modes, error lines), config parse happy/invalid. Bench verification per
TASK_autopid.md Phase 1.
