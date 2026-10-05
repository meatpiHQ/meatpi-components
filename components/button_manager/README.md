# button_manager: the hardware button (GPIO8)

Two product behaviors ride the one button (meatpi 2026-07-19; legacy
`safe_mode_check` + `config_mode.c` semantics, both POLLED: interrupts
are banned on this pin by ruling):

| Gesture | Behavior | Owner |
|---|---|---|
| **Held while power is applied** | **SAFE MODE**: bare recovery boot, default AP `WiCAN_<mac>` / `@meatpi#` @ 192.168.0.10 (always the factory identity, whatever the stored settings say), a small server (recovery page, `/upload_firmware` multipart OTA, `/factory_reset` = raw settings-partition + NVS erase, and since 2026-10-05 `/crash_report` + `/crash_report.txt`: the crash report the restart tracker keeps in NVS, shown on the page with Copy and Download; the factory reset puts it back after the NVS erase), NO settings load, 10 min no-client timeout-reboot. LED: sky-blue during the 5 s decision window, solid yellow in safe mode (legacy). | `main/main_safemode.c`: runs INSTEAD of the composition, so it lives in the root, not here |
| **Held on a PARKED device** (2026-10-05: LED breathing red, the crash-loop brake) | hold until the LED turns sky blue (about 3 s: the park saw the press and restarted) and release = **one normal start**; keep holding until yellow = safe mode, with the crash report. Polled once per 2 s nap, then held 1 s awake: no interrupt, no wake source on the pin. | `main/main_park.c`: it runs instead of the composition too |
| **Long-press while running** (default 5 s) | **CONFIG MODE**: BLE stopped, AP forced up with the *configured* AP identity whatever the running mode (`wifi_manager_config_ap()`, the one sanctioned runtime mode change), LED alternating yellow/blue at 1 Hz (legacy), 10 min timeout-reboot back to the configured state: held open while an AP client is attached. | detection HERE; policy in `main/main_glue.c` (`main_glue_wire_button`) |

This component itself is deliberately tiny: a 1 s `gpio_get_level` poll
task (the legacy cadence, no ISR), a pure hold-tracking state machine,
and ONE callback fired exactly once per press. It knows no other
component; what a long-press *does* is composition policy.

## API

- `button_manager_init()`: settings + log registration.
- `button_manager_start()`: GPIO (input, pull-up, polled) + the task.
  Disabled via settings → logs and idles.
- `button_manager_set_longpress_cb(cb)`: main wires the config-mode
  glue. The callback runs on the button task (4 KB **internal** stack:
  it reaches radio teardown paths, §2).

## Settings (`"button_manager"`, v1)

| Key | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `true` | runtime long-press detection (safe mode is NOT gated by this: it runs before settings exist) |
| `hold_s` | int 1..30 | `5` | seconds of continuous hold (legacy `CONFIG_MODE_HOLD_SECONDS`) |

No CLI commands.

## Memory footprint

| Where | What | ~Size |
|---|---|---|
| Internal `.bss` | poll-task stack (radio-path callback, §2) + TCB | ~4.4 KB |

## Tests

- **Host (`host_test/`, 6 tests)**: the pure press state machine,
  threshold, one-shot per press, release re-arm, bounce reset.
- **On-device**: needs a human finger, manual procedure: (1) hold the
  button while plugging in → sky-blue LED, keep holding 5 s → yellow LED
  + `WiCAN_<mac>` AP on air at 192.168.0.10 with the recovery page; the
  factory-reset and OTA buttons work; idle 10 min → normal reboot.
  (2) With the device running, hold 5 s → LED alternates yellow/blue,
  the configured AP appears even in STA-only mode, BLE drops; idle
  10 min (no AP client) → reboots back to the configured state.
