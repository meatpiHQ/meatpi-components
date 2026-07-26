# event_manager

The device automation engine (design: [TASK_event_manager.md](TASK_event_manager.md)):
components PUBLISH typed events and REGISTER named actions and pull
values; user RULES (settings, UI-edited) wire them together. The
manager knows no component by name — the bridge_manager ownership
inversion. Each participating component owns a `<comp>_events.c`
(the `<comp>_cli.c` shape).

```
SOURCES ──publish──▶ queue ──dispatcher──▶ match/when/cooldown ──▶ ACTIONS
                                │                                    ▲
                          RULES (settings)              ${…} templates + pull VALUES
```

## Rules (settings `event_manager`, reboot-to-apply)

```json
{ "enabled": true,
  "timers": [{"name": "beat", "period_s": 15}],
  "rules": [
    {"name": "high_rpm", "on": "autopid.param", "match": {"param": "rpm"},
     "when": [{"key": "value", "op": ">", "val": 3000}],
     "do": "mqtt.publish",
     "with": {"topic": "~/alerts", "payload": "{\"rpm\":${value}}"},
     "cooldown_ms": 60000}
  ]}
```

`on` = `source.event` (must exist in the registry); `match` = typed
equality pre-filter; `when` = AND-ed conditions
(`== != > >= < <= changed contains`; OR = write a second rule);
`do`+`with` = one action, `${key}` templates resolve event values,
builtins (`ts`/`source`/`name`) and pull values (`${autopid.data}`,
`${autopid.<param>}`); `cooldown_ms` rate-limits. `changed` is
per-boot. Validation is strict at PUT time (registries populated);
timers publish `timer.tick {timer}` on esp_timer (64-bit µs — no
32-bit tick types anywhere here).

## v1 sources / actions / values

| Owner | Events | Actions / values |
|---|---|---|
| event_manager | `timer.tick {timer}` | `log.note {message}` |
| mqtt_manager | `mqtt.rx {topic, payload≤47ch}` — topics AUTO-derived from enabled rules' `match.topic` at start (hot-source pre-filter) | `mqtt.publish {topic, payload, qos, retain}` (async path; `~/x` = prefix-relative) |
| autopid | `autopid.param {param,value,unit,group}` (ON CHANGE + `min_event_interval_ms`), `autopid.pid_failed {pid,streak}`, `autopid.scan_done {found}` | action `autopid.group {group,enabled[,period_ms]}` (§5b context switching — EPHEMERAL, reboot restores config defaults); values `${autopid.data}` (legacy JSON snapshot) + `${autopid.<param>}` |
| battery_monitor | `battery.threshold {edge, volts}` (12.0 V down / 12.5 V up, 5 s hold; initial state delivered at boot) | value `${battery.voltage}` |
| imu_manager | `imu.motion {state}` (active\|stationary), `imu.bump {axes}` | — |
| dev_status_manager | `status.bit {bit, set}` on EVERY bit change (sta_connected, ble_connected, mqtt_connected, motion, time_synced, …) — context rules for free | — |
| rtc_manager | — | values `${time.iso}` (ISO8601 UTC) + `${time.epoch}` |
| led_manager | — | `led.indicate {r,g,b,mode}` / `led.clear` (the ALERT slot only) |
| http_client_manager | — | `http.post {url, body, content_type?}` (no retries; errors counted; blocking → worker pool) |
| obd_chip | `obd.response {cmd, response≤47ch}` (published by the action) | `obd.request {cmd}` — chain a rule on obd.response to deliver the answer (blocking → worker pool) |
| uds_manager | `uds.response {ok, nrc, len, data≤15B, req}` (published by the action) | `uds.request {tx, rx, req, ext?}` — outcome-only; full payloads via a script's `uds()` binding (blocking → worker pool) |

**Default rules** (schema default; they REPLACED `main_events.c`):
`imu.bump` / `imu.motion` / `battery.threshold` → `mqtt.publish
~/events` with the legacy-glue payload shapes (`{"event":"bump",…}`,
`motion_active|stationary`, `battery_below|above` + `${time.iso}`).

Still planned (TASK §5): wifi.sta {connected,ssid} + ble.client
sources (status.bit covers the connected-bit cases today), the
`time.at` calendar source, a dedicated target app.

## Behavior notes

- Publish NEVER blocks: queue depth 32, full = drop-oldest + counter.
- Two action lanes, chosen per-action by `em_action_t.blocking`:
  - **inline** (`blocking=false`, the default) — runs on the dispatcher
    task (8 KB PSRAM). Fast, non-network actions: `log.note`,
    `mqtt.publish` (already async), `led.*`, `autopid.group`.
  - **worker pool** (`blocking=true`) — a SLOW network/bus round-trip
    (`http.post`, `obd.request`, `uds.request`) is handed to a bounded
    pool (`EM_WORKERS=2` tasks, 4 KB PSRAM each) via a depth-8 job queue,
    so one 8 s HTTP+TLS post can't stall tick processing. The queue is
    drop-OLDEST when full (`stats.blocking_dropped` counts the drops);
    the rendered `with` cJSON ownership transfers dispatcher→worker.
    `run()` on a blocking action MUST NOT touch flash (PSRAM-stack task,
    §2 corollary) — all v1 blocking actions are network/bus only.
- No flash access, no retries on any lane — errors are counted
  (`stats.action_errors`), not retried. Boot-window publishes before
  the broker connects count there by design.
- Legacy destination parity is RULES, not code: the cycle-MQTT
  publish = `on timer.tick → mqtt.publish {payload:"${autopid.data}"}`
  (bench-verified).

## HTTP (see `components/HTTP_API.md`)

`GET /api/events/sources|actions|values` — the UI's rule-editor
vocabulary (zero hardcoded dropdowns). `GET /api/events/log` — stats +
the last 32 events with which rules fired (the rule-debugging view).

## Files

| File | Role |
|---|---|
| `event_manager.c` | queue, dispatcher, rules+settings, timers, lifecycle |
| `event_manager_registry.c` | source/action/value registries, kv constructors, the log ring, JSON builders |
| `event_manager_rules.c` | PURE parse + match/when/cooldown (host-tested) |
| `event_manager_template.c` | PURE `${key}` substitution (host-tested) |
| `event_manager_http.c` | the four routes |
