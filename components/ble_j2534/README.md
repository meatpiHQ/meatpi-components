# ble_j2534

> Wire protocol: `j2534_server/J2534_WIRE_PROTOCOL.md` (section 8.3 is the
> BLE binding). GATT surface: `ble_manager/BLE_API.md`.

## Summary

J2534 PassThru over BLE: the glue between a `ble_manager` stream channel
(`j2534`, **FFF5 indicate / FFF6 write** under service 0xFFF0; the
channel registers `out_modes = INDICATE` only, because the J2534 wire
protocol has no credit or frame-counter message to make notifications
loss-detectable, see `ble_http` for that design) and
`j2534_server`'s transport seam. It registers the channel (pre-start,
only when `j2534_server.enabled` is true, so a disabled feature exposes
nothing on air) and runs ONE tester session per secured BLE link from its
own PSRAM-stacked task through `j2534_server_serve_transport()`, exactly
the shape of `usb_cdc_device`'s CDC-ACM session task. The single-tester
rule and the Exclusive-bus hold apply unchanged; `/api/j2534` shows
`"transport":"ble"`.

## API

| Function | Purpose |
|---|---|
| `ble_j2534_init()` | log descriptor + channel registration (iff `j2534_server_is_enabled()`). After `j2534_server_init()`, BEFORE `ble_manager_start()` |
| `ble_j2534_start()` | creates the session task (no-op when not registered). After `j2534_server_start()` |
| `ble_j2534_stop()` | sleep path: ends an active session, stops the task |
| `ble_j2534_status(out)` | registered / link_up / session_active / sessions |

## Dependencies

`ble_manager` (channel API), `j2534_server` (`j2534_transport_t`,
`serve_transport`), `log_manager`.

## Settings

None. Gated by `j2534_server.enabled`; `j2534_server.allow_lan` applies
to the TCP listener only (a paired BLE link is a local link, like the AP
or the USB cable).

## Memory footprint (estimated)

| What | Size |
|---|---|
| channel RX StreamBuffer storage (two maximal requests) | 9216 B PSRAM |
| session task stack | 4096 B PSRAM (+ TCB internal) |
| flash | ~2 KB |

No `j2534_msg_t` on this stack (the server's scratch is its own PSRAM
statics). No flash writes.

## Testing

`tools/testbench/ble/ble_j2534_pi.py` on rpi001 (bleak; run through
`ble_pi_run.py` = `test.ps1 blej2534`) -> `BLE J2534 PASS`: HELLO/OPEN,
CONNECT CAN + `7DF 02 01 00` answered by the ECU simulator from 7E8 (with
the `/api/can` tx counter as witness), PASS/BLOCK filters, ISO15765
`22 F1 87` (ECU name) and `10 02`, the reflash gate, a second tester over
TCP refused with `ERR_DEVICE_IN_USE`, BLE drop mid-session releasing the
server (`transport:"none"` within 5 s, a TCP session then succeeds),
second BLE session clean. **2026-09-21: PASS** (HELLO RTT p50 188 ms /
p95 300 ms over the bench link). The TCP regressions `j2534_bench.py`
(`J2534 TARGET PASS`) and `j2534_gate_test.py` (`J2534 GATE TEST PASS`)
passed the same day on the refactored transport seam. `TESTING.md` row
`blej2534`.
