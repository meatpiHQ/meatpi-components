# log_manager: on-target test app

Self-contained, **two-phase across a real reboot**. Builds against the main
partition table (rev 2.1). Run:

```powershell
.\test.ps1 target log_manager
```

## What is covered

Composition boot (log pipeline first, settings applied, task started), a
custom sink receiving `ESP_LOGx` output end-to-end, runtime level control
(DEBUG gated at INFO, passes after `set_level`), a quiet tag (a descriptor
below INFO keeps its level across a wildcard set, is let through when raised
by name and is quiet again after the next wildcard; the level table's
occupancy), sink disable, **drop-oldest
backpressure** (300-line burst from a priority-5 task (above the log task)
must drop, never block), and the PSRAM ring surviving a genuine
`esp_restart()` (a unique marker logged before the reset is found in the ring
after it), then ring clear.

## Expected result: serial markers

Phase 1 (fresh flash: ring holds no phase marker):

```
INIT ok=1
SINK saw_probe=1 lines_gt0=1
LEVEL filtered=1 passed=1
QUIET kept=1 raised=1 again=1
LEVELS used=2 cap_ge_64=1
DISABLE unchanged=1
BACKPRESSURE dropped_gt0=1
PHASE1 rebooting
```

then the device reboots itself, and phase 2 prints:

```
INIT ok=1
RING survived=1 has_boot_mark=1
RINGCLEAR empty=1
TEST DONE
```

Phase 2 clears the ring, so a third boot runs phase 1 again (repeatable).
Last verified: 2026-07-03 on WiCAN Pro.
