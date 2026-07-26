# interface_manager

THE place wireless-interface on/off decisions live (policy component,
meatpi 2026-07-05 — generalizes the legacy wireless modes ap+sta / ap /
sta / sta+BLE into rules).

## Rules (v1)

| Rule | Behavior |
|---|---|
| `sta_ble_handover` | A BLE client connecting means the user is in the car: **STA suspends** for the drive. When the BLE client leaves (back home, phone BLE off), STA reconnects; BLE keeps advertising. The legacy "sta+BLE" mode. |
| `ap_ble_exclusive` | AP and BLE both advertise while idle; the side the user **connects** to wins: BLE client → AP suspends; AP station → BLE stops. The loser returns when the winner disconnects. BLE wins a tie (the drive signal outranks a lingering AP association). |

Rules only engage when BLE is enabled and the wifi mode includes the
interface in question. Future rules (e.g. USB-connected turns both
radios off) extend `im_inputs_t` + `im_policy_evaluate()` — nothing
else changes.

## Shipping defaults

Fresh device: wifi `mode=ap` (onboarding access point), BLE
`enabled=false`. Enabling BLE puts the device in the AP+BLE
arbitration; configuring STA engages the handover rule.

## Mechanics

A 4 KB internal-stack task polls every 500 ms (dev-status bits
`BLE_ENABLED`/`BLE_CONNECTED` + `wifi_manager_get_ap_station_count()`),
evaluates the PURE rule table (`interface_manager_policy.c`,
host-tested 7/7), debounces (2 identical evaluations ≈ 1 s) and diffs
the target against current suspensions. Actuators:

- `wifi_manager_suspend_sta()/resume_sta()` and `suspend_ap()/resume_ap()`
  — new RUNTIME per-interface suspension in wifi_manager
  (`wifi_manager_suspend.c`): recomputes the esp_wifi mode from
  (configured mode − suspensions), parks the reconnect task while STA
  is suspended, respects `ap_auto_disable`. EPHEMERAL — settings
  untouched, reboot restores.
- `ble_manager_stop()/start()` for the BLE side.

Policy inputs use `ble_manager_is_enabled()` — the CONFIGURED truth,
stable across runtime stop/start (the `BLE_ENABLED` dev-status bit
tracks the *running* state and clears when the policy stops the stack).
The current arbitration state is published as dev-status bits
`sta_suspended` / `ap_suspended` / `ble_suspended` (BIT20–22), visible
in `/api/status` and the `status` CLI command — the answer to "why is
this interface down". `interface_manager_stop()` resumes everything
(interfaces are never left suspended without their arbiter).

## Settings (`interface_manager`, reboot-to-apply)

| Field | Default | Meaning |
|---|---|---|
| `enabled` | `true` | master switch for the arbitration task |
| `sta_ble_handover` | `true` | rule toggle |
| `ap_ble_exclusive` | `true` | rule toggle |

## Verified live (2026-07-05, bench: apsta + BLE on)

BLE client connects → `STA suspended` + `AP suspended` within ~1.5 s
(device confirmed off-network from the host); BLE disconnects → both
resumed, STA pingable again in ~6 s. The AP-station-wins leg is
host-tested (joining the DUT's AP from the bench Pi would kill the
bench hotspot).

## Memory

Task stack 4 KB internal (actuations can hit NVS/flash paths); policy
state is a few bytes. No heap use at runtime.
