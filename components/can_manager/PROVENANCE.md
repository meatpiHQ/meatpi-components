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
