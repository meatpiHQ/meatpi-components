# battery_monitor — battery/supply voltage owner (feature)

Owns the battery-sense ADC input (WiCAN Pro: ADC1 CH3 = GPIO4 behind a
÷11 divider, 6 dB attenuation — all Kconfig per hardware revision:
`WICAN_BATT_ADC_CHANNEL` / `WICAN_BATT_DIVIDER_X100` /
`WICAN_BATT_OFFSET_MV`). Samples every `poll_s`, 8-sample averaged,
eFuse-calibration corrected (curve fitting, line-fitting fallback — the
IDF v6 oneshot API; the legacy continuous/DMA path is gone).

This is the **foundation for sleep_manager** (its sleep/wake voltages
become one watch) — and a general trigger source: stop ECU polling on a
sag, MQTT alert on low battery, etc.

## API

| Call | Behavior |
|---|---|
| `battery_monitor_init()` | Settings + log descriptors. No ADC. |
| `battery_monitor_start()` | ADC + calibration up, one synchronous reading (boot log shows the supply), sampler task. `ESP_ERR_INVALID_STATE` when unconfigured. |
| `battery_monitor_voltage(*v)` | Latest averaged volts; `ESP_ERR_INVALID_STATE` before the first sample. |
| `battery_monitor_watch(cfg, q, *id)` | Register a threshold watch (below). |
| `battery_monitor_unwatch(id)` | Remove it. |
| `battery_monitor_register_http()` | `GET /api/battery` → `{"voltage":11.56}` (main wires it; the ADC code has no HTTP dep). |

## Watches (register a threshold, subscribe to events)

```c
QueueHandle_t q = xQueueCreate(4, sizeof(battery_monitor_event_t));
battery_monitor_watch(&(battery_monitor_watch_cfg_t){
    .below_v = 12.0f,   /* BELOW when sustained under this            */
    .above_v = 12.5f,   /* ABOVE when sustained at/over this          */
    .hold_ms = 5000,    /* debounce: the side must persist this long  */
}, q, &id);
```

Semantics (pure policy, host-tested):

- **Hysteresis pair**: `below_v`..`above_v` is a silent band — no
  flapping around a single threshold (engine cranking dips, alternator
  ripple).
- **Hold debounce**: a side must persist `hold_ms` before its event
  fires; a dip back into the band restarts the hold.
- **Initial-state delivery**: the first settled side after registration
  is delivered too, so consumers learn the current state without
  polling.
- Fan-out matches imu_manager: zero-timeout queue sends, drop-and-count,
  the sampler never blocks. Events carry the watch id (one queue can
  serve several watches), the voltage, and the uptime.
- Up to 8 watches (`BATTERY_MONITOR_MAX_WATCHES`).

## Settings (`"battery_monitor"`, version 1, field table)

`cli` (bool, default true): register this component's console command(s) with cmdline_manager on the settings boot apply (reboot-to-apply). Ownership: the component registers its own commands — main wires nothing (2026-07-05).

`enabled` (bool, true) · `poll_s` (1..60, default 3 — the legacy
cadence). Thresholds are NOT settings here: consumers own their numbers
(sleep_manager will persist its own sleep/wake voltages and register
them as a watch).

## Files

- `battery_monitor.c` — lifecycle, settings, watch registry, sampler.
- `battery_monitor_policy.c` — PURE watch state machine (host-tested,
  6 tests: hysteresis, hold, initial state, clock wrap).
- `battery_monitor_adc.c` — oneshot + calibration + divider scaling
  (ported from legacy sleep_mode.c; the ×11 + 0.1 V bench numbers).
- `battery_monitor_http.c` — the optional route.

## Verified

2026-07-04 on the bench (PSU set to 11.3–11.7 V): boot log
`started (11.56 V, poll 3s, ch3, div x11.00)`; `GET /api/battery` →
`{"voltage":11.51}`. Gotcha found: concurrent `adc_oneshot_read` calls
fail on the unit lock — the synchronous boot reading runs BEFORE the
sampler task exists.

## Memory

Sampler task 3 KB PSRAM stack; watch table in PSRAM `.bss` (~300 B);
ADC driver handles internal. (estimated)

## CLI

`battery_monitor_register_cli()` (main, CLI builds) registers the `battery` command with cmdline_manager (`battery_monitor_cli.c`).
