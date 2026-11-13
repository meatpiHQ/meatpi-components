# MicroLink

Vendored Tailscale client used by `vpn_manager`. The upstream layout and
WiCAN-specific changes are recorded in [PROVENANCE.md](PROVENANCE.md).
The composition initializes NVS, networking and the uplink before starting it.
Dependencies are declared in `CMakeLists.txt`; the WiCAN integration uses
`wireguard_lwip`, `ip4_routes`, mbedTLS and the shared flash-job hook.
Persisted product settings belong to `vpn_manager`, not this component.

## Lifecycle

- `microlink_init(config)`: allocate the client and load its keys.
- `microlink_start(client)`: start asynchronous connection tasks. Refuses an
  instance that still owns tasks, including after a timed-out stop.
- `microlink_stop(client)`: request shutdown and join the four tasks.
  `ESP_ERR_TIMEOUT` leaves every handle and resource intact for a retry.
  The relay task closes its own connections as it ends (the TLS close stays
  on its stack, never on the caller's); a successful stop then drains what
  was queued for the relay after that.
- `microlink_destroy(client)`: now returns `esp_err_t`. `ESP_OK` means the
  instance was freed (NULL is also accepted). On timeout it is still owned
  by the caller: retain the pointer and retry, never start a replacement.

The remaining API is in [include/microlink.h](include/microlink.h).

## Stalled DERP shutdown

The rule: a client is never destroyed, and no task of a new client is
started, while a task of the old client still runs. The four TCBs are
statics and the stacks are freed by destroy, so breaking it hands a running
task's TCB and stack to someone else.

The DERP connection/reconnection loops check shutdown after their retry delay.
They must not open another connection after a stop request. On beta.9 a retry
did exactly that, the 30-second join expired, and stop cleared the live task's
handle anyway. The second stop inside destroy then saw no tasks and freed the
instance and its PSRAM stacks. The next client reused its static TCB while an
old lwIP callback was still pending.

Cancelling the retries is not enough by itself: one dial that is already in
flight when the uplink goes blocks in `connect()` for about a minute, so a
stop can still miss its deadline (seen on the fix, 2026-11-13 18:09: two
deadlines missed, `destroy deferred` twice, the client destroyed 61.7 s
after the stop request and only after the relay task had ended, a new client
connected, no crash). Keeping the client on a timed-out stop is what makes
that safe.

It is also what the dials no longer do. `ml_dial()` (`src/ml_dial.c`, the
relay's dials and the control task's) is a non-blocking connect waited for in
200 ms slices that look at the stop request first: 10 s at most, and gone
within a slice of a stop. With it the client is destroyed 0.2 to 0.5 s after
the stop is asked for, and the VPN is back about 15 s after a Wi-Fi blip
instead of about 70. The kept client stays as the net under it
(PROVENANCE item 25).

Reproduced on the exact beta.9 image `c69c8478ea78bd97` with a dropped relay
dial and a Wi-Fi trial: delayed LoadProhibited crashes in lwIP task-wakeup
callbacks, also with MQTT disabled. The separately reported `ml_derp_tx`
stack-overflow signature has not been reproduced; no stack-size increase is
being used as a substitute for fixing ownership.

Until the same date nothing closed the relay connections at a stop: a relay
still connected when the stop came kept its socket and its TLS context
(279,564 B of PSRAM and 4,380 B of internal RAM over four stops, measured by
leaving the close out). The relay task closes them itself now, as it ends,
and logs `DERP I/O task exiting (stack: N B never used)`. The WireGuard
device (about 17.5 KB of PSRAM) was not freed either, at any stop: the
manager task's exit now ends with `wireguardif_fini()`. With both, the PSRAM
free behind four stops in a row read 2,814,552, 2,814,508, 2,814,144 and
2,814,148 B. The full record is PROVENANCE item 24.

## Dependencies

`wireguard_lwip` (the shared WireGuard core), `ip4_routes`, mbedTLS (PSA),
lwIP, NVS, `espressif/cjson`; the caller's flash hook (`flash_run` in the
config) carries every NVS access off the PSRAM stacks. No settings of its
own and no console commands: both belong to `vpn_manager`.

## Memory

The four main stacks remain 8/14/12/7 KiB in PSRAM (net_io, derp_tx, coord,
wg_mgr), with four internal static TCBs. The 2026-11-13 change adds no stack
or buffer allocation. On a timed-out stop, existing resources intentionally
stay allocated until the tasks really exit. A stop and a start of the client
leave nothing behind (measured: the figures above; internal RAM within 300 B
over four stops). Other client buffers and measured
RAM costs are described in PROVENANCE and `vpn_manager/README.md`.

## Tests

- Firmware checkout: `.\test.ps1 target microlink`. One real-FreeRTOS
  regression holds a task across two 30-second stop deadlines and checks that
  stop, start and destroy preserve ownership. See [test_apps/README.md](test_apps/README.md).
- `.\test.ps1 tspublic stop-dial`: the actual network-loss reproduction,
  including a delayed-callback watch, zero errors and settings restoration.
- `.\test.ps1 tspublic`: relay/direct traffic, rekey and expired-session
  recovery, task headroom and no restart through the soak.
