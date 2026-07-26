# obd_chip bench — setup, verification, and facts (Coding Standard §7)

> Status: **bench verified 2026-07-03** with the `bench_probe` app (lived in
> `components/obd_chip_manager/test_apps/`, since superseded by this
> directory's real `obd_chip` test app and deleted 2026-07-05 — the "flash
> the probe" step below now means flashing THIS test app; the sleep-pin
> release sequence it demonstrated lives in `obd_chip_uart.c`). Everything
> below was measured on the real bench, not assumed.

## Verified bench topology

```
  PC ──USB──┬─ CH342 ch A = COM7 → ESP32 UART0 (console/flash, 2 M in main fw)
            └─ CH342 ch B = COM6 → ESP32 UART2 (GPIO17 TX / GPIO18 RX, 115200)
                                          │ (transparent bridge in probe app)
  ESP32 UART1 (GPIO16 TX / GPIO15 RX) ── OBD chip (ELM327 v2.3, 2,000,000 baud)
                                          │ CAN 500 kbit/s, 11-bit
        ECU simulator ────────────────────┤  (answers 0x7E8, e.g. 0100 → 41 00 FF FF FF FF)
        PCAN-USB FD **channel 2** ────────┘  (PCAN_USBBUS2 — observer/injector;
                                              channel 1 is NOT on this bus)
```

## Hardware facts (measured / from meatpi)

| Item | Value |
|---|---|
| Chip UART | UART1, TX = GPIO16, RX = GPIO15 |
| Chip baud | **2,000,000** live on this bench (default 115200; legacy switches via `STSBR`) |
| Chip identity | `ATI` → `ELM327 v2.3`, `>` prompt, `\r` endings, echo on by default, `VT*` vendor commands |
| USB bridge B | UART2, TX = GPIO17, RX = GPIO18 ↔ **COM6** @ 115200 |
| Sleep pin | GPIO9, output; **high = awake, low = sleep**. The sleep path parks it low with pulldown + `gpio_hold_en` + RTC pulldown — **waking REQUIRES the release sequence** (`gpio_hold_dis` → `rtc_gpio_deinit` → reset/reconfig; see `bench_probe_main.c`, from meatpi) |
| Ready pin | GPIO7, input, pulldown (chip-driven; spec says high = active) |
| CAN | 500 kbit/s, 11-bit; ECU simulator live; PCAN observer/injector on `PCAN_USBBUS2` |

**Open observations (for meatpi, task §11):**
- `READY` (GPIO7) read **0** while the chip was awake and answering — polarity,
  timing, or meaning differs from "high = active"; needs clarification.
- Legacy code references an `OBD_RESET_PIN` whose GPIO number isn't in this
  repo — needed for the hardware-reset path.
- `PCAN_USBBUS1` (channel 1 of the dual PCAN) is not wired to this bus.

## How to verify the bench (repeatable)

1. Flash the probe: build this app (`idf.py set-target esp32s3 && idf.py build`,
   flash to COM7). Serial markers expected:

   ```
   PINS sleep=1 ready=<0|1>
   CHIP ok=1 baud=2000000 ready=<0|1> resp=ATI.ELM327 v2.3..>
   BRIDGE READY uart2@115200 <-> uart1@2000000
   TEST DONE
   ```

   (`baud=115200` after a chip power cycle is also a pass; `ok=0` is a fail —
   check the wake sequence and wiring.)

2. PC side (bridge + CAN + ECU sim in one command):

   ```powershell
   python tools\testbench\obd_bench_check.py          # defaults: COM6, PCAN_USBBUS2
   ```

   Expected:

   ```
   PASS  bridge: COM6 -> chip replied: 'ATI.ELM327 v2.3..>'
   PASS  pcan: PCAN_USBBUS2: ECU sim answered 0x7E8 data=06 41 00 ff ff ff ff 00
   ```

   The probe's bridge keeps running after TEST DONE, so any serial terminal on
   COM6 is a live chip console (handy for exploring the AT/VT dialect).

## Tools + versions

| Tool | Version |
|---|---|
| ECU simulator | on-bench unit (model TBD from meatpi), 500k/11-bit, standard PIDs |
| PCAN-USB FD | PCANBasic driver installed; python-can 4.x, interface `pcan` |
| USB bridge | CH342 dual (channels COM7/COM6) |
| python | IDF v6.0.2 venv + pyserial + python-can |

## Pass/fail criteria (bench health)

- Probe markers as above, `CHIP ok=1`.
- `obd_bench_check.py` exits 0 (both PASS lines).
- Anything else: consult the observations list before debugging the firmware.
