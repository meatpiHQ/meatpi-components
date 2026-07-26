# api_http — the device HTTP API glue (Phase 7)

Implements the core components' `/api` routes on top of `http_server_manager`
per **`components/HTTP_API.md`** (conventions + route map) and the
per-component endpoint references. Core components never touch HTTP; this
glue depends **down** on them (Architecture §10). Feature components
(`wifi_manager`, …) register their own routes — not here.

## API

| Call | Behavior |
|---|---|
| `api_http_init()` | Registers all routes with http_server_manager (buffered until the server starts) + the log descriptor. Call after `http_server_manager_init()`. |
| `api_http_start()` / `api_http_stop()` | Lifecycle uniformity (§3); passive after init. `start()` refuses with `ESP_ERR_INVALID_STATE` if init never ran. |

## Routes implemented (authoritative detail lives next to each component)

- **`/api/settings*`** (`settings_manager/HTTP_API.md`): list, GET with
  password redaction + `degraded` flag, full-replace PUT (`""` password =
  keep stored; validated; `changed` dedup), schema GET,
  `POST /api/settings/submit` → `{"reboot":bool}` then a ~1 s-deferred
  `restart_tracker_restart(CONFIG_APPLY, CONFIG_SERVER)` iff anything changed.
- **`/api/status`** (`dev_status_manager/HTTP_API.md`): every named bit,
  `network_connected`, uptime, version/partition, boot counters.
- **`/api/info`** (`dev_status_manager/HTTP_API.md`): device identity for
  the HA integration's control-API check (device-contract v2 ask #1) —
  `device_type`/`model`/`hw_version`/`fw_version`/`device_id`/`mac`/
  `api_level`.
- **`/api/ota/upload` + `/api/ota/status`** (`ota_manager/HTTP_API.md`):
  multipart (`firmware`/`ota_file`) or raw-body upload → deferred reboot;
  plus the legacy alias `POST /upload/ota.bin` (the HA-integration
  migration bridge, same handler).
- **`/api/restart/history` + `POST /api/restart`**
  (`restart_tracker/HTTP_API.md`): forensics ring newest-first with
  `*_to_str` names; reboot responds `{"ok":true}` first, then
  `restart_tracker_restart(USER_REQUEST, WEB_UI, flags)` — never raw
  `esp_restart()`.
- **`/api/logs/*`** (`log_manager/HTTP_API.md`): chunked `text/plain` ring
  dump (PSRAM snapshot → internal-RAM chunk buffer per §9.4), ring clear,
  dropped/sink status, ephemeral level + sink toggles.
- **`/api/fs/*`** (`filesystem/HTTP_API.md`): read-only list/info; path
  validation is the component's own (400/404/503 mapping).

Registration order matters once: exact URIs (`/api/settings/submit`,
`/api/settings`) register before the `/api/settings/*` wildcards — the
wildcard matcher takes the first hit.

## Reboot semantics

One shared deferred-reboot path (`api_schedule_reboot`): respond → ~1 s
flush delay (dedicated internal-stack task) → `restart_tracker_restart`.
First scheduled reboot wins; later requests are no-ops (the device is going
down anyway).

## Dependencies

`http_server_manager`, `settings_manager`, `dev_status_manager`,
`restart_tracker`, `log_manager`, `filesystem` — all private;
`espressif/cjson` (managed, private — the public header exposes only
`esp_err_t`). Init order: after `http_server_manager_init()`, before
`http_server_manager_start()` (later also works — routes install live).

## Memory footprint (estimated — measure before release)

| Where | What | ~Size |
|---|---|---|
| PSRAM `.bss` | ring-dump snapshot buffer | 20 KB |
| Internal `.bss` | wire chunk buffer (1 KB) + mutex/TCB | ~2 KB |
| Heap (transient) | PUT bodies (PSRAM, ≤8 KB), cJSON trees, reboot task stack (3 KB internal, one-shot) | request-scoped |

## Tests

- **Host (`host_test/`, 7 tests)**: the pure utility layer — password
  redaction/unredaction (`""` keeps stored), settings route parsing
  (name/schema/rejects), log-level mapping.
- **Target (`test_apps/`)**: full composed stack on the lwIP loopback,
  TWO-PHASE (reboots once through the real submit path) — see
  `test_apps/README.md`. **Green on hardware 2026-07-03.**
