# sleep_manager

Low-power manager (rewrite of legacy `sleep_mode.c`,
`TASK_sleep_manager.md`). Battery voltage — read from
**battery_monitor**, this component owns no ADC — drives a pure
ladder: `NORMAL → LOW_VOLTAGE` (below `sleep_mv`, countdown of
`sleep_delay_min`) `→ SLEEPING → WAKE_PENDING` (recovered ≥
`wake_mv`, stable for `wake_delay_ms`) `→ reboot` via restart_tracker POWER_WAKE
(PERIODIC_WAKE for a check-in, 2026-09-07 — the Status page shows which) —
wake-by-reboot, not resume-in-place (every manager restarts clean).

SLEEPING = repeated **light sleep** with a 2 s timer wake; voltage is
still sampled between naps and each nap verifies the OBD chip stayed
asleep (≤6 re-sleeps, then an INTERNAL_RECOVERY reboot). Optional
periodic check-in reboot (`periodic_wakeup` + `wakeup_interval_min`),
gated below 11.9 V so a dying battery is never drained for telemetry.

Boot-loop guard (legacy parity): ≥3 unexpected resets while the
battery reads under 12.1 V → sleep instead of another crash lap.

**Critical floor (Ali, 2026-10-01):** a battery that reads under
**11.90 V for 120 s** (`SM_CRITICAL_V`, `SM_CRITICAL_DELAY_MS`, fixed)
puts the device to sleep **whatever the `enabled` setting says** and
without waiting out the user's sleep delay; the normal wake rules apply
(wake voltage + hold, no periodic check-in under the floor). A device
booting on such a battery still comes up normally (15 s grace) and
stays reachable for the 120 s. The pure tracker `sm_critical_eval()`
(hysteresis 0.05 V) is host-tested; the sleep bench's `critical_floor`
leg and the matrix scenario of the same name prove it with sleep
disabled at 11.75 V.

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

## Settings (`sleep_manager`, v4, reboot-to-apply)

`enabled` (**true** — shipping default ON since 2026-07-18, 5 min delay:
a parked device must not drain the car battery out of the box),
`sleep_mv` (12000–14000, 13100), `wake_mv` (12100–15000, 13200; v3,
2026-10-01), `wake_delay_ms` (100–5000, 500; v4, 2026-10-01: "wake up
after", how long the battery must stay above `wake_mv` before the wake
reboot; was a fixed 1 s; the Power Saving page shows it in seconds),
`sleep_delay_min` (1–30, 5), `periodic_wakeup` (false),
`wakeup_interval_min` (5–1440, 30), `cli` (true).

The wake voltage used to be derived (sleep + 0.1 V, legacy). Since v3 it
is the user's own number, measured on the car by Quick Setup's "Battery
and sleep" step (charging voltage with the engine running, resting
voltage after key-off) or set on the Power Saving page. The pure
`sm_resolve_thresholds()` pulls a wake value that is not at least
0.1 V above sleep up to that band (one W line at boot); `on_migrate`
gives a v1/v2 document `wake_mv = sleep_mv + 100` (`sm_migrated_wake_mv()`),
so a configured device keeps its behaviour. autopid's "pause at the sleep
voltage" resumes polling at this wake voltage (one pair rules both).

## Surfaces

- `GET /api/sleep` — `{enabled, state, voltage, sleep_v, wake_v,
  naps}` (the UI's sleep banner).
- CLI `sleep` (status) / `sleep test <secs>`.
- Events: `sleep.entering {volts}`, `sleep.state {state, volts}`.
- dev_status: sets/clears WAKE_VOLTAGE_OK continuously; AWAKE→SLEEP
  on entry.

## Footprint

PSRAM: 4 KB task stack. Internal: FreeRTOS objects only. Host tests:
`host_test/` (13 tests: the pure ladder incl. clock wrap, the wake hold
following `wake_delay_ms`, the critical floor tracker, the v3 threshold
resolver, the v2 -> v3 and v3 -> v4 migration values). Bench:
`sleep test` smoke (entry sequence + naps + POWER_WAKE reboot);
`tools/testbench/sleep/sleep_bench_test.py` (`.\test.ps1 sleep`) walks
the supply across sleep and wake, including a wake-voltage leg (a step
between sleep and wake must NOT wake the device) that also times the wake
with a 5 s hold against the default 0.5 s.
