# can_core — adopted code

`can_core.c` / `can_core_filter.c` / `can_core_recovery.c` (+ headers).
Lived as the standalone `elm327_can` component until 2026-07-19, when it
was merged into can_manager and renamed (the `elm327_` prefix mislabeled
the open CAN core; the component boundary leaked through can_manager's
public headers anyway).

Adopted 2026-07-07 from an earlier meatpi project: the shared CAN (TWAI)
bus core — ONE handle per peripheral; clients ({rx_cb, filter, mask,
monitor_all}) and task-owned RX queues (drop-oldest) register into it;
normal TX, bus-wide silent mode, stats. Uses the LEGACY `driver/twai.h`
API (still shipped in IDF v6.0.2; the `esp_driver_twai` node-API
migration is future work).

Sole owner in this repo: `can_manager` (v6 lifecycle/settings/pins/
standby around this core — the vpn_manager-over-esp_wireguard layering).
Nothing else may call `can_core_init` (one handle per peripheral).

## Local changes (this repo)

1. RX dispatch task moved to a PSRAM stack (static-create: PSRAM stack
   buffer + internal TCB). The task does `twai_receive` + queue/callback
   dispatch only — never flash — so the §2 corollary allows it; frees
   `CAN_CORE_RX_TASK_STACK` bytes of the scarce internal heap (needed
   for tailscale's NVS-touching `wg_mgr` task to start alongside USB
   host + CAN, bench 2026-07-07).
2. Ported to the `esp_driver_twai` node API (2026-07-21); the line about
   the legacy `driver/twai.h` API above is history.
3. The node driver half (ISR callbacks, the static raw RX queue, node
   bring-up on the configured ISR core, teardown) lives in
   `can_core_driver.c` / `can_core_driver.h` since 2026-10-02 (700-line
   rule); `can_core.c` keeps the RX task, dispatch, TX, stats and the
   reconfiguration entry points. Behaviour unchanged.
4. Listen before talk (2026-10-03, README "Listen before talk"): the node
   always starts listen-only WITHOUT its TX pin, a pure policy
   (`can_autobaud_core.c`) proves the bitrate from frames, walks the
   candidates of `baud=auto`, promotes and demotes; `can_core_link.c`
   applies it from the RX task, gates `can_core_transmit()` and holds
   `can_core_set_baud()` / `can_core_set_silent_mode()`, which now go
   through the policy (a runtime request to talk is honoured only once the
   bitrate is proven or a fixed-bitrate bus stayed silent). Callers of the
   three, the private `esp327` add-on included, see a refused transmit
   (`ELM327_ERR_BUSY` after the caller's own timeout, counted in
   `tx_refused`) instead of a frame at the wrong bitrate.
5. The ISR counts into internal RAM (`rx_missed`, `rx_bad`, the errors by
   kind): the TWAI interrupt is cache-safe and the handle lives in PSRAM.
6. Explicit bit timing (`can_timing_core.c`: 20 quanta, 80 %, SJW 3) through
   `twai_node_reconfig_timing()`; the node API's own choice below
   500 kbit/s mis-read frames on the bench.
7. The client / queue-subscriber tables live in `can_core_clients.c` since
   2026-10-03 (700-line rule): 16 queue subscribers, each with a name and
   its own drop counter.
8. `fail_retry_cnt = 0` (it read 512, an `int8_t` that truncated to 0):
   single shot, as it always behaved.
