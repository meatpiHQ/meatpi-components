# api_http — on-target test app (self-contained, TWO-PHASE)

Composes the full core stack the way main will (log_manager →
restart_tracker → dev_status_manager → filesystem → settings_manager →
http_server_manager → api_http), serves on the **lwIP loopback**, and
asserts every `/api` area with `esp_http_client` against 127.0.0.1 — no
external network or instruments. Run:

```powershell
.\test.ps1 target api_http
```

## What is covered

A registered test settings component (`api_test`: int `value` + string
`api_password`, default `topsecret`) proves the settings pipeline
end-to-end: list, redacted GET (+`degraded:false`), schema, 404 on unknown
component, schema-validation 400 with error text, PUT `changed:true`, the
`""`-password-keeps-stored rule (identical second PUT dedups to
`changed:false`), and **the real submit-then-reboot**: `POST
/api/settings/submit` answers `{"reboot":true}` and the device reboots via
`restart_tracker_restart(CONFIG_APPLY, CONFIG_SERVER)`. Phase 2 (marker
file `/data/api_test_phase`) verifies the value persisted, the restart
history records the planned `config_apply`/`config_server` reboot, and a
no-op submit answers `{"reboot":false}` without rebooting.

Also covered: `/api/status` (bits incl. a set `awake`, uptime, boot
counters), `/api/restart/history`, `/api/logs/status|level|sink|ring`
(incl. 400 unknown level, 404 unknown sink, non-empty chunked ring dump),
and `/api/fs/list|info` (incl. traversal rejection).

## Expected result — serial markers, in this order

```
INIT ok=1
PHASE 1
SETTINGS-LIST ok=1 has_api_test=1
SETTINGS-GET ok=1 degraded0=1 redacted=1
SCHEMA ok=1 has_props=1
SETTINGS-404 ok=1
PUT-BAD rejected=1 has_err=1
STATUS ok=1 awake=1 uptime=1 boots=1
HISTORY ok=1 has_records=1
LOGS-STATUS ok=1 has_console=1 has_ring=1
LOGS-LEVEL ok=1
LOGS-LEVEL-BAD rejected=1
LOGS-SINK-404 ok=1
LOGS-RING ok=1 bytes_gt0=1
FS-LIST ok=1 has_probe=1
FS-LIST-BAD rejected=1
FS-INFO ok=1 has_total=1
PUT-OK ok=1 changed=1
PUT-KEEP noop=1
SUBMIT ok=1 reboot=1
REBOOTING (config_apply via restart_tracker)
   ... device reboots (~1 s after the submit response) ...
INIT ok=1
PHASE 2 (after submit reboot)
PHASE2-PERSISTED ok=1 value7=1
PHASE2-HISTORY ok=1 planned=1 reason=1 source=1
PHASE2-NOOP-SUBMIT ok=1 noreboot=1
TEST DONE
```

Phase 2 cleans up (restores `api_test` defaults, deletes the marker), so
re-runs behave identically. Last verified green: 2026-07-03 on WiCAN Pro.

## Not covered here

`/api/wifi/*` — registered by wifi_manager itself
(`wifi_manager_register_http()`, HTTP_API.md §7); needs the radio, covered
by the WiFi HIL S10 over-RF scenario. `POST /api/restart` shares the
deferred-reboot path asserted via submit (a second reboot would double the
suite time for the same code path).
