# sleep_manager

Low-power manager (rewrite of legacy `sleep_mode.c`,
`TASK_sleep_manager.md`). Battery voltage — read from
**battery_monitor**, this component owns no ADC — drives a pure
ladder: `NORMAL → LOW_VOLTAGE` (below `sleep_mv`, countdown of
`sleep_delay_min`) `→ SLEEPING → WAKE_PENDING` (recovered ≥
sleep + 0.1 V, stable 1 s) `→ reboot` via restart_tracker POWER_WAKE
(PERIODIC_WAKE for a check-in, 2026-09-07 — the Status page shows which) —
wake-by-reboot, not resume-in-place (every manager restarts clean).

SLEEPING = repeated **light sleep** with a 2 s timer wake; voltage is
still sampled between naps and each nap verifies the OBD chip stayed
asleep (≤6 re-sleeps, then an INTERNAL_RECOVERY reboot). Optional
periodic check-in reboot (`periodic_wakeup` + `wakeup_interval_min`),
gated below 11.9 V so a dying battery is never drained for telemetry.

Boot-loop guard (legacy parity): ≥3 unexpected resets while the
battery reads under 12.1 V → sleep instead of another crash lap.

## Entry sequence

`sleep.entering {volts}` event (rules get one last MQTT/HTTP shot,
BEFORE radios stop) → wait for autopid idle (≤20 s, only if enabled) →
**main's prepare callback** (`main/main_sleep.c`: autopid,
data_logger, mqtt, vpn, mdns, wifi, ble, external_storage — the
composition root owns the order, so this component depends on none of
them) → CAN transceiver standby (GPIO38) → obd_chip_sleep(true) → USB
power rail held low (GPIO10, open-drain + hold). Pins via Kconfig
(`WICAN_SLEEP_*_GPIO`, defaults = WiCAN Pro).

## Bench safety (meatpi 2026-07-07, mandatory)

- The state task arms only after a **15 s boot grace** — a bootloop
  still leaves a flash window.
- All sleeping is **timer-woken light sleep** — there is no state
  only an external signal can leave.
- `sleep test <secs>` (CLI) / `sleep_manager_test_sleep()` force the
  full entry sequence with a timed wake — the USB-powered bench unit
  (which never sees a "recovered" voltage) always comes back.

## Settings (`sleep_manager`, v1, reboot-to-apply)

`enabled` (**true** — shipping default ON since 2026-07-18, 5 min delay:
a parked device must not drain the car battery out of the box),
`sleep_mv` (12000–14000, 13100), `sleep_delay_min`
(1–30, 5), `periodic_wakeup` (false), `wakeup_interval_min` (5–1440,
30), `cli` (true). Wake threshold is derived: sleep + 0.1 V (legacy
decision).

## Surfaces

- `GET /api/sleep` — `{enabled, state, voltage, sleep_v, wake_v,
  naps}` (the UI's sleep banner).
- CLI `sleep` (status) / `sleep test <secs>`.
- Events: `sleep.entering {volts}`, `sleep.state {state, volts}`.
- dev_status: sets/clears WAKE_VOLTAGE_OK continuously; AWAKE→SLEEP
  on entry.

## Footprint

PSRAM: 4 KB task stack. Internal: FreeRTOS objects only. Host tests:
`host_test/` (7 tests, the pure ladder incl. clock wrap). Bench:
`sleep test` smoke (entry sequence + naps + POWER_WAKE reboot).
