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

## API

The public header is [include/microlink.h](include/microlink.h). The
lifecycle is the four functions above; the rest, one line each:

- `microlink_factory_reset()`: erase the device keys and the peer cache.
- `microlink_get_state(client)`, `microlink_is_connected(client)`: the
  client's state; connected means its control session is up.
- `microlink_derp_connected(client)`, `microlink_derp_home_region(client)`:
  the home relay connection, and the region this node is reachable at.
- `microlink_get_vpn_ip(client)`: the tailnet address (0 before the map).
- `microlink_get_peer_count(client)`, `microlink_get_peer_info(client, i,
  out)`: the peer table (a slot bound: a pruned slot answers an error).
- `microlink_get_routes(client, out, max)`: the subnet routes learned from
  the tailnet.
- `microlink_send(client, ip, data, len)`: one datagram to a peer.
- `microlink_set_state_callback`, `microlink_set_peer_callback`,
  `microlink_set_data_callback`: optional callbacks (unused on WiCAN).
- `microlink_ip_to_str(ip, buf)`, `microlink_parse_ip(str)`,
  `microlink_default_device_name()`, `microlink_imei_device_name()`,
  `microlink_resolve(client, hostname)`: helpers.
- `microlink_udp_*` (close, send, recv, set_rx_callback, get_local_port) and
  `microlink_tcp_*` (connect, send, recv, is_connected, close): upstream's
  socket helpers over the tunnel, built and unused on WiCAN (the firmware
  reaches the tunnel through lwIP's own sockets and `ip4_routes`).

A change of this header is noted in it: `microlink_destroy` returns
`esp_err_t` since 2026-11-13, and `microlink_stop` may return
`ESP_ERR_TIMEOUT` with the client still owned.

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

Nothing is allocated while no client exists. With a client (figures of the
bench, 2026-11-13, one peer, the home relay and one peer's region open):

| What | Where | Size |
|---|---|---|
| task stacks: net_io 8, derp_tx 14, coord 12, wg_mgr 7 KiB | PSRAM, heap, freed by destroy | 41 KiB (configured) |
| the four TCBs | internal, static | `4 * sizeof(StaticTask_t)` |
| init: the client's state, queues, keys | internal heap | 8,424 B (measured, the `tailscale starting` line) |
| start: the UDP socket, the tasks | internal heap | 380 to 528 B (measured, same line) |
| the map reassembly buffer | PSRAM, at the first long-poll data | 512 KiB (`CONFIG_ML_JSON_BUFFER_SIZE_KB`) |
| the H2 accumulation buffer | PSRAM, at the first long-poll frame | 64 KiB (configured) |
| a relay connection's TLS context | PSRAM | about 52 KB each (estimated: 209,440 B over four connections left open) |
| the WireGuard device (the peer table) | PSRAM | about 17.5 KB (measured: what stayed behind a stop before it was freed) |

Stack use: the relay task ends with 9.9 to 10.1 KB of its 14 KiB never used
(measured, its own exit line, after TLS handshakes and its own close);
`ml_dial()` has a 96 B frame (measured, the compiler's `.su`).

The 2026-11-13 changes add no stack and no buffer. On a timed-out stop,
existing resources intentionally stay allocated until the tasks really exit.
A stop and a start of the client leave nothing behind (measured: PSRAM free
2,814,552 and 2,814,148 B behind the first and the fourth stop in a row,
internal RAM within 300 B). The history of the client's RAM cost is in
PROVENANCE and `vpn_manager/README.md`.

## Tests

- Firmware checkout: `.\test.ps1 target microlink`. One real-FreeRTOS
  regression holds a task across two 30-second stop deadlines and checks that
  stop, start and destroy preserve ownership. See [test_apps/README.md](test_apps/README.md).
- `.\test.ps1 tspublic stop-dial`: the actual network-loss reproduction,
  including a delayed-callback watch, zero errors and settings restoration.
- `.\test.ps1 tspublic`: relay/direct traffic, rekey and expired-session
  recovery, task headroom and no restart through the soak.
