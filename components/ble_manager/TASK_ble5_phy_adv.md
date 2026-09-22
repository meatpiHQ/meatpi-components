# TASK: BLE 5 as a user choice (PHY + advertising sets), 2026-09-21

## Why

Ali: BLE 4.2 vs 5 should be selectable in settings, not at compile time, and
the difference between the 1M and 2M PHY must be benchmarked. Measurement
(same day, `idf.py size`): NimBLE's 5.0 feature set is already compiled in
(`CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y`, the S3 controller supports 5.0 in
hardware); the only feature compiled out is extended advertising, and
compiling it in costs **+1.8 KB flash, 0 B static internal RAM** (NimBLE
allocates from PSRAM here). So: compile it in once, choose at runtime.

## Contract (what the user gets)

Two new `ble_manager` settings (descriptor v4, reboot-to-apply, defaults =
today's on-air behaviour so every 4.2 phone keeps working):

| Key | Values | Default | Effect |
|---|---|---|---|
| `phy` | `1m`, `2m`, `coded`, `auto` | `1m` | the PHY the device PREFERS on a link: set as the controller default at bring-up and requested right after connect (`ble_gap_set_prefered_le_phy`). The central decides; a 4.2 peer keeps 1M. `auto` = 1M or 2M, whichever the peer negotiates |
| `advertising` | `legacy`, `extended`, `both` | `legacy` | which advertising set(s) run: the legacy PDU set every scanner sees (today's bytes: flags + tx power + the FFF0 128-bit UUID, name + `MeatPi` in the scan response), an extended set (5.0 scanners only: one PDU carrying flags + UUID + name + mfg data, connectable, primary 1M, secondary per `phy`), or both at once |

`GET /api/ble` gains `phy` (setting), `advertising` (setting), `phy_tx` /
`phy_rx` (the live link's PHYs, 1 / 2 / 3 = 1M / 2M / coded, 0 when not
connected). Documented in BLE_API.md 1, 6, 7 and HTTP_API.md 6e15.

## Design

- `sdkconfig.defaults` + `sdkconfig`: `CONFIG_BT_NIMBLE_EXT_ADV=y`,
  `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=2`, `CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE=256`.
  With `BLE_EXT_ADV` on, NimBLE compiles `ble_gap_adv_start()` into an
  ENOTSUP stub, so advertising moves to the extended API for BOTH sets:
  instance 0 = `legacy_pdu` (IND: connectable + scannable), instance 1 =
  extended (connectable, non-scannable, `primary_phy` 1M, `secondary_phy`
  from `phy`). Same on-air bytes for the legacy set (verified by discovery
  in the bench: name, UUID, `MeatPi`).
- New file `ble_manager_gatt_adv.c` (NimBLE only, keeps `gatt_nimble.c`
  under 700 lines): builds the AD payloads with `ble_hs_adv_set_fields_mbuf`,
  configures/starts/stops the instances, handles
  `BLE_GAP_EVENT_ADV_COMPLETE`. On CONNECT every instance stops (one
  central); on DISCONNECT the configured set(s) restart.
- PHY: `ble_gap_set_prefered_default_le_phy(mask, mask)` at bring-up,
  `ble_gap_set_prefered_le_phy(conn, mask, mask, BLE_GAP_LE_PHY_CODED_ANY)`
  after connect when `phy != 1m`; `BLE_GAP_EVENT_PHY_UPDATE_COMPLETE`
  logged at I and mirrored into `/api/ble` (`ble_gap_read_le_phy`).
- Pure helpers (host-tested, `ble_manager_pack.c`): `blm_ident_phy_mask()`
  (`1m`->1, `2m`->2, `coded`->4, `auto`->1|2, unknown->1; a strict mask lets the LL fall back to 1M on its own when the peer cannot) and
  `blm_ident_adv_mode()`.
- Settings: fields `phy` / `advertising` (`SETTINGS_STR_ENUM`), version 3 ->
  4, `on_migrate` stays the no-op (fill-missing defaults); `blm_config_t`
  gains `phy_mask` + `adv_mode`.
- Standard: §6c (BLE_API.md updated in the same change), §4 (field table,
  version bump + migrate note), §10 (I for PHY changes, W for a refused
  PHY request, never E), README API/Settings/Memory/Testing rows.

## Bench (the 1M vs 2M measurement)

`tools/testbench/ble/ble_phy_bench_pi.py` (ON rpi001, `test.ps1 blephy`),
verdict `BLE PHY PASS`. For `phy` in (`1m`, `2m`): PUT + submit-reboot,
connect + pair, read `/api/ble` `phy_tx`/`phy_rx` (expect 1/1 then 2/2:
the UB500 is a 5.0 adapter), then through the HTTP tunnel: 3 x 64 KB
download (KB/s, byte-exact), 1 x 64 KB upload (KB/s), 10 x `GET
/api/status` RTT; `METRIC` per PHY, then the ratio. Also `advertising`:
`legacy` (bonded connect + a fresh scan finds `WiC_<id>` after dropping the
bond) and `both` (same discovery; the extended set is visible to the
5.0-capable UB500 as a second advertising report). Restore stage puts the
saved settings back. PASS = 2M negotiated on the 2M leg, every transfer
byte-exact on both, discovery works in both advertising modes; the
throughput ratio is REPORTED (coex on the bench link makes the absolute
numbers small).

## Results (2026-09-21, `test.ps1 blephy` PASS, 517 s, 3 reboots)

Measured from the PC's Intel adapter (the bench Pi's UB500 cannot hold a
2M link with the S3, see below), `sta_ble_handover` ON (WiFi suspended
during the link, the product behaviour), MTU 517 / 490 B payload:

| | 1M | 2M |
|---|---|---|
| link PHY (`GET /api/ble` over the tunnel) | 1M / 1M | 2M / 2M |
| 64 KB upload, write-without-response | 50.6 KB/s | 51.9 KB/s |
| 64 KB download x3, indications, byte-exact | 5.58 KB/s | 5.69 KB/s |
| `GET /api/status` RTT p50 (10x) | 484 ms | 531 ms |
| channel counters | 0 tx_timeouts, 0 rx_overflow, 0 resyncs | same |

Advertising (btmon on the Pi, bond dropped, fresh scan): `legacy` = 2
legacy / 0 extended reports carrying `WiC_<id>`, the FFF0 UUID and
`MeatPi`; `both` = 3 legacy + 1 extended report (secondary PHY LE 2M).
Restore clean: 0 own-tag E lines, 0 new faults, 0 unexpected resets.

**Reading.** 2M buys nothing measurable on this device today because
neither direction is air-time bound: uploads already run at ~50 KB/s on
1M (the earlier 1.7-2 KB/s figures were the Pi's bleak/BlueZ D-Bus
WriteValue path: btmon shows 222 ms between write commands), and
downloads are ONE 490 B indication per ATT round trip: the central
confirms in 0.3 ms but the device sends the next indication a median
148 ms (p90 380 ms) later (`tools/testbench/ble/ble_phy_trace_pi.py`),
so 2.5-5.7 KB/s whatever the PHY, and a tunnelled GET costs ~0.5 s.
Making downloads fast needs the channel OUT direction on notifications
with app-side credits (a protocol change to BLE_HTTP_PROTOCOL.md and the
stream-channel conventions; the earlier notification path lost 2 of 146
PDUs device-side, which is the thing to fix first). Owner's decision.

**Firmware traps this bench found (fixed in the same change).**
1. `BLE_HS_ADV_TX_PWR_LVL_AUTO` in the AD makes NimBLE send the legacy
   LE_Read_Advertising_Channel_Tx_Power HCI command, which the controller
   answers *Command Disallowed* once it runs the extended API: the first
   build advertised NOTHING (`legacy advertising set config failed (524)`).
   The AD now carries the configured level.
2. `phy = 1m` must SET the 1M-only default preference: with it unset the
   PC's Intel adapter switched the link to 2M on its own (`phy_tx 2M`
   while the setting said `1m`). Every value now sets the preference; the
   host-side per-link request follows only when the controller did not
   already switch.
3. The connect-time `ble_gap_security_initiate()` (legacy parity) made
   Windows start a system pairing that an app's own WinRT ceremony could
   not join (`Failed`, no ceremony asked; the DUT saw ENOTCONN 2 s in).
   Removed: centrals pair on first encrypted access, as phones always
   did; a Windows app pairs with `ProvidePin` + `accept_with_pin()`.
   `pairing failed` now logs the SMP status at W.
4. Deferring the PHY request to CONN_UPDATE / ENC_CHANGE (so the 4 s
   supervision timeout is in force during the switch) was tried against
   the UB500 drop and did not help; kept because it removes a redundant
   procedure at connect.

**Rig limit.** TP-Link UB500 (RTL8761BU, fw 0xdfc6d922, BlueZ 5.66,
kernel 6.12): the PHY update to 2M completes and 90-300 ms later BOTH
sides report Connection Timeout (0x08): no packet gets through on 2M in
either direction, WiFi on or off, legacy or extended advertising. The
bench keeps `1m` for that adapter (WARN `adv_2m_ub500_2m_drop`) and runs
the PHY legs from the PC (bleak 3.0.2 in the v5.5.3 venv, installed with
`--trusted-host`: the v6.0.2 venv's pip fails on a self-signed cert).

Memory: bin 3,513,888 B (+8.8 KB vs the pre-BLE5 build incl. the adv
file, PHY handling and the `/api/ble` fields); static DIRAM unchanged
(`.dram0.data` 35,479 B + `.dram0.bss` 124,344 B); the extended set adds
nothing while `advertising = legacy`.
