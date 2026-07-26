# bridge_endpoints

The adapter layer between providers and `bridge_manager`: every data
interface wrapped as a named JACK (bridge endpoint). Providers stay
bridge-agnostic; this component knows both sides — it is the one
component that may depend on bridge_manager AND the providers
(ARCHITECTURE §10; formerly `main/main_endpoints.c` glue).

| Jack | Wraps | Registered |
|---|---|---|
| `obd` | `obd_chip` (MIC3624; **multi_consumer** — the chip's RX fan-out copies every chunk to every subscriber, TX serialized, so obd may sit in several enabled bridges: TCP+USB+WS defaults) | init (fixed) |
| `can` | `can_manager` — frame⇄chunk pump task, owned here (**multi_consumer** since 2026-07-26: up to 4 bridges at once — slcan + mqtt_can together; RX chunks fan out to every subscriber, TX serializes via `can_manager_send`; ONE jack-wide ingress filter `bep_can_set_filter`, per-consumer filters out of scope) | init (fixed) |
| `ble` | `ble_manager` GATT pipe | init (fixed) |
| `cli` | `cmdline_manager` | init (fixed) |
| `usb_obd` | USB port-B UART (UART2 TX17/RX18 @2 Mbaud), owned here | init (fixed) |
| socket servers | `socket_manager` — one jack PER CONFIGURED server, under its **configured name** (defaults `obd0 slcan0 gvret0 udp0`) | start (dynamic) |
| WS channels | `websocket_manager` — one jack per configured channel (defaults `ws_obd ws_can ws_cli ws_log`; slots for all 6 channels since wsm v2) | start (dynamic) |

Add-on component packs may register further jacks straight with
bridge_manager during their ext init hook (same pre-start window).
Dynamic socket/WS jacks follow the settings names — renaming a server
renames its jack; a name colliding with an existing jack is refused by
the registry and logged (that jack alone degrades).

## API

- `bridge_endpoints_init()` — log descriptor + the 5 fixed jacks.
- `bridge_endpoints_start()` — dynamic socket/WS jacks (reads the applied
  settings names) + USB port-B UART bring-up.
- `bridge_endpoints_stop()` — no-op (jacks live as long as the registry).

## Dependencies / ordering

`bridge_manager`, `obd_chip`, `ble_manager`, `can_manager` (+
`can_core` frame type), `cmdline_manager`,
`socket_manager`, `websocket_manager`, `log_manager`, `esp_driver_uart` —
all private. main composes: `init` after `bridge_manager_init`; `start`
after the settings boot pass and BEFORE `bridge_manager_start`
(endpoint registration is pre-start only).

## Settings

None. Which jacks carry traffic is bridge_manager's `bridges[]` config;
the socket/WS jack NAMES come from those components' own settings.

## Memory footprint (estimated — measure before release)

| Where | What | ~Size |
|---|---|---|
| Internal `.bss` | USB RX task stack (3 KB, UART driver path §2) + TCBs | ~3.5 KB |
| Internal heap | UART2 driver rings (4 KB + 4 KB) at start | ~8 KB |
| PSRAM `.bss` | CAN pump stack (3 KB) + name buffers | ~3.2 KB |
| Internal heap | CAN RX queue (32 × frame) on first `can` subscribe | ~0.5 KB |

## Tests

Glue — no host suite (thin wrappers + two FreeRTOS adapters with no
isolatable pure logic; the frame⇄chunk codec it uses is host-tested in
`can_manager`'s `can_frame_wire`). Covered live by the bridge benches:
translator suites (slcan/gvret TCP+WS) and the USB passthrough leg.
