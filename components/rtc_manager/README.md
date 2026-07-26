# rtc_manager — battery-backed RTC owner (service)

Owns the RX8130CE RTC (I2C 0x32 on `i2c_bus`). The chip's backup caps keep
time through reboots and short unpowered periods, so timestamps are sane
seconds after power-on with no network:

- **start()** (every boot AND deep-sleep wake — both come through
  app_main): if the RTC holds a plausible time (2020..2099, valid
  fields — a fresh board / drained caps reads 2000-01-01 or garbage and
  is REJECTED), the system clock is restored from it and
  `DEV_STATUS_BIT_TIME_SYNCED` is set.
- **SNTP task** (settings-gated): syncs the system clock AND writes the
  RTC back on **every internet (re)connect** — the task watches the
  network bits and re-syncs within seconds of each reconnect edge (not
  just the first connect) — plus a periodic resync every
  `sync_interval_h` while the link stays up (60 s retry after a failed
  attempt). Live-verified: hotspot bounce → STA rejoin → resync 2 s
  later.

**Time is UTC everywhere** (RTC and system clock). Timezone is a
presentation concern of the consumer — legacy's `worldtimeapi.org` HTTP
lookup is intentionally gone (dead dependency, and local-time RTCs make
DST math everyone's problem).

## API

| Call | Behavior |
|---|---|
| `rtc_manager_init()` | Settings + log descriptors. No bus traffic. |
| `rtc_manager_start()` | Chip bring-up (legacy control-register values — they configure backup-cap charging, do not touch), clock restore, SNTP task. |
| `rtc_manager_get_time(*tm)` | RTC as UTC broken-down time; `ESP_ERR_INVALID_ARG` when implausible. |
| `rtc_manager_sync_from_system()` | Persist the system clock into the RTC (the one write path). |
| `rtc_manager_now_iso8601(buf, len)` | `YYYY-MM-DDTHH:MM:SSZ` from the system clock. |
| `rtc_manager_set_time(epoch)` | Manual set (UTC seconds, 2020..2099): system clock + RTC + TIME_SYNCED — the no-NTP path. |
| `rtc_manager_time_valid()` | Clock trusted this boot (RTC restore, SNTP, or manual). |
| `rtc_manager_sync_now()` | Blocking on-demand SNTP sync (primary + fallback). |
| `rtc_manager_last_sync()` / `_ntp_server()` / `_sntp_enabled()` | Status getters for transports. |
| `rtc_manager_register_http()` | `GET /api/rtc` (time + validity + chip time + sntp status) / `POST /api/rtc {"epoch":N}` / `POST /api/rtc/sync` (on-demand) — HTTP_API.md §6d; main wires it. |

## Decided semantics

- Chip reads use the coherent double-read pattern (burst twice until the
  seconds register agrees — a rollover between bursts would tear the date).
- Chip access serializes on a mutex; the SNTP task runs on a PSRAM stack
  (network only, no flash writes).
- TIME_SYNCED means "the system clock is trustworthy", whether it came
  from the RTC or NTP — consumers (logging timestamps) don't care which.

## Settings (`"rtc_manager"`, version 1, field table)

`cli` (bool, default true): register this component's console command(s) with cmdline_manager on the settings boot apply (reboot-to-apply). Ownership: the component registers its own commands — main wires nothing (2026-07-05).

`enabled` (bool, true) · `sntp` (bool, true) · `ntp_server` (str,
"pool.ntp.org" — user-configurable; live-verified against
time.google.com) · `ntp_server2` (str, "" — optional fallback tried
when the primary fails; live-verified: bad primary → fallback synced) ·
`sync_interval_h` (1..168, default 24).

Legacy `sync_sys_time.c` review (2026-07-04): adopted its explicit
`TZ=UTC0` pin (no component can skew the UTC-everywhere model), its
multi-server redundancy (as the configurable fallback), and its
sanity floor — a server answering with a pre-2020 date is ignored, so
a bogus NTP reply can't poison the clock or the RTC.

## Files

- `rtc_manager.c` — lifecycle, settings, restore, SNTP task.
- `rtc_manager_time.c` — PURE BCD/struct-tm codec + plausibility gate
  (host-tested, 5 tests).
- `rtc_manager_rx8130.c` — chip layer.

## Memory

SNTP task 4 KB PSRAM stack; state few dozen bytes. (estimated)

## CLI

`rtc_manager_register_cli()` (main, CLI builds) registers the `rtc [sync]` command with cmdline_manager (`rtc_manager_cli.c`).
