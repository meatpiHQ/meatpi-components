# obd_chip — on-target test app (live bench)

Runs against the **real bench**: chip + ECU simulator at 500k/11-bit + PCAN —
topology, wiring, tools and versions are documented in `BENCH.md` next to
this file (the §7 bench doc, verified 2026-07-03). Run:

```powershell
.\test.ps1 target obd_chip
```

## What is covered

Composition boot (settings→start with real 2 M baud negotiation and chip
reset), request→response (`ATI`, `VTVERS`), a standard PID against the ECU
simulator (`0100`), the **canonical multi-frame VIN** (`0902`, multi-line),
fan-out to a subscriber, the slow-consumer drop policy (depth-1 queue drops,
healthy subscriber unaffected), claim arbitration (MONITOR blocks `request`,
release restores), monitor-class classification + `request()` refusal, a real
`ATMA` session terminated by the SPACE stop byte, and staging the embedded
vendor firmware file to `/data`. After `TEST DONE` the app keeps a **USB
bridge** (COM6 ⇄ chip, built on `subscribe`/`send` — the production
passthrough shape) and a console command loop.

## Expected result — serial markers, in this order

```
INIT ok=1
START ok=1 ready=1 ready_pin=<0|1>
REQ ati_ok=1 elm=1
VERSION ok=1 mic=1 resp=MIC3624 V2.3.22
PID0100 ok=1 has41=1
VIN ok=1 lines=4 has4902=1
FANOUT chunks=1 gt0=1
DROPS tiny=1 gt0=1 probe_intact=1
CLAIM blocked=1
CLAIM released_ok=1
MONITOR classified=1 refused=1
ATMA stopped_ok=1
FWSTAGE ok=1 bytes=494257
BRIDGE READY
TEST DONE
```

(`ready_pin` reads 0 on the current bench while the chip answers — the open
READY-semantics question; the version string tracks the installed chip fw.
Since the 2026-07-26 async bring-up, `START` prints after the app has waited
for `obd_chip_ready()` — `ready=1` is the real bring-up outcome.)
Last verified: 2026-07-03 on the live bench.

## Console commands (after TEST DONE, on COM7 @115200)

- `VERSION` → `FWVERSION MIC3624 Vx.y.z`
- `FWUPDATE` / `FWUPDATE FORCE` → chip firmware update from the staged file
  (`/data/obd_fw/latest.txt`, embedded V2.3.22). **EXCLUSIVE while running;
  do not interrupt power.** Ends with `FWUPDATE done err=...`.

**Real update verified 2026-07-03**: `FWUPDATE FORCE` reflashed V2.3.22 on
the bench chip — 3557 records in ~80 s, `err=ESP_OK`, and the full marker
suite (incl. live-ECU PID 0100 + VIN) green on the next boot. Note the
`FWVERSION` printed right after the update may be blank (the chip was just
hardware-reset); reset the DUT and read the `VERSION` marker instead. If an
update is interrupted mid-stream the chip stays in download mode (`VTVERS`
answers `?`, hardware reset does not exit it) — running `FWUPDATE FORCE`
again recovers it; this path is exercised and verified.

## PCAN scenarios (via the bridge + PCAN_USBBUS2)

With the bridge running, `tools/testbench/obd_bench_check.py` re-verifies the
chip and the ECU path from the PC.

**4 KB ISO-TP `VT` transmit — VERIFIED 2026-07-03** with a REAL ISO-TP
stack (python-can + **can-isotp**, per the task spec): run
`tools/testbench/obd_vt_isotp_check.py` (expects `VT ISO-TP CHECK PASS`,
`--size` selectable). It sends `VTFullyRequestCk<LLLL><hex><CCCC>` through
COM6 while an `isotp.CanStack` receiver (rxid 0x7DF, FC txid 0x7E8,
BS=0/STmin=0) owns flow control, sequence checking and reassembly — any
ISO-15765-2 violation by the chip surfaces through the stack's error
handler. Verified byte-exact with zero stack errors at 7 B (single frame),
100 B and the full 4095 B. Syntax + the real-world response transcript
(J1850 VPW `ATSH`, UDS TransferData, per-message output) — and why the
`IT CH_NO`/`OK` lines in the raw tester log are the Bluetooth tester box,
not the chip: component `README.md` §Open stubs item 3 (source:
`wican_pro_tester/protocol_notes.md`). The trailing ` <n>` is the ELM
expected-response-count hint; it makes no observable difference against
the single-response bench sim (`1`/`2`/none probed identical).

**Monitor-injection — VERIFIED 2026-07-03**: run
`tools/testbench/obd_monitor_injection_check.py` (expects
`MONITOR INJECTION CHECK PASS`). It enables `ATH1`, starts `ATMA`, injects
0x123/`DE AD BE EF 11 22 33 44` from PCAN, asserts the frames stream through
the bridge (the `<DATA ERROR` tag on arbitrary data is expected — not a
failure), and stops the session with SPACE.
