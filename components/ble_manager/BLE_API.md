# WiCAN Pro — BLE GATT interface reference

> The on-air contract for third-party apps (Android / iOS / desktop) —
> the BLE counterpart of `components/HTTP_API.md` (API-first, Architecture
> §1b: anyone can build their own app; there is no privileged client).
> Byte-exact with the legacy WiCAN Pro firmware: existing apps keep
> working unchanged. Facts below are from the implementation
> (`ble_manager_gatt_nimble.c` / `ble_manager_gatt.c` — both backends
> serve identical bytes) and were verified on air against BlueZ/bleak
> (`tools/testbench/ble_bench.py`, `ble_ab.py` — working client code to
> crib from).

## 1. Discovery

| | |
|---|---|
| Advertised name | `WiC_<device_id>` — `device_id` = 12 lowercase hex (SoftAP MAC), e.g. `WiC_14c19f44e349` (legacy ble_uid format; serial 2A25 = name+7 = last 9 hex) |
| Advertised service UUID | `0000FFF0-0000-1000-8000-00805F9B34FB` (128-bit, complete list) |
| Advertising interval | 160 ms, general discoverable, connectable |
| Address | **Resolvable private address** (rotates). Scan by NAME or the advertised service UUID, never by a hard-coded MAC |
| TX power | `tx_power_dbm` setting (default 9 dBm) |

BLE is **disabled by default** (`ble_manager.enabled = false`) — the user
enables it via `/api/settings/ble_manager` (reboot to apply). Advertising
stops while a client is connected (one central at a time) and resumes on
disconnect.

## 2. Pairing & security

- **LE Secure Connections + MITM**, device role DisplayOnly: the phone
  prompts for a **6-digit passkey** — default `123456`, the `passkey`
  setting. The device initiates security right after connect.
- **Every** characteristic requires an **encrypted + authenticated**
  link (`ENC` + `AUTHEN` on reads/writes, incl. Device Info) — an unpaired
  connection can discover the service/characteristic layout but read or
  write nothing (answered *Insufficient Authentication*, which prompts
  pairing).
- **Bonding** (`bonding` setting, default true, v2): when on, the pairing
  exchanges long-term keys so a paired phone reconnects **without
  re-entering the passkey** — and those keys are **persisted to NVS**
  (`CONFIG_BT_NIMBLE_NVS_PERSIST`, `ble_store_config_init()` at bring-up),
  so the bond survives reboots and power-cycles. Turn bonding **off** for
  a stricter posture — the link is still encrypted + MITM-protected every
  session, but no reusable long-term key is distributed or kept, so every
  reconnect re-pairs and nothing reusable is left on the device. Applied
  as `ble_hs_cfg.sm_bonding` (+ key-distribution) at stack bring-up;
  reboot to apply. Up to `MAX_BONDS` (3) phones; a 4th evicts the oldest
  (round-robin, `ble_store_util_status_rr`).
- **Secure Connections only** (`sc_only` setting, default true, v3):
  refuses to pair with a peer that tries to downgrade to weak LE *legacy*
  pairing (its static-passkey exchange is brute-forceable offline). Every
  phone since ~2014 does LE Secure Connections, so the interop cost is
  negligible — turn it off only for a genuinely pre-4.2 accessory. Applied
  as `ble_hs_cfg.sm_sc_only`.
- **No characteristic is readable on an unpaired link** — the Device Info
  service (0x180A) now also requires `READ_ENC | READ_AUTHEN`, alongside
  the data (FFF1) and CLI-OUT pipes. An unpaired read returns *Insufficient
  Authentication*, which prompts the client to pair (meatpi 2026-07-08).
- **Pairing window**: `pairing_at_boot` (default true) opens the window
  at boot; firmware can close it at runtime
  (`ble_manager_pairing_allow/deny`). Window closed = NEW pairings are
  rejected; a reconnect by an already-bonded phone (using its stored key)
  is unaffected.
- **Bond loss recovery**: with NVS persistence the device now keeps its
  bond across reboots, so the common "phone bonded, device forgot after
  restart" mismatch is gone. It still happens after a **re-flash /
  factory reset** (which clears NVS) — then the phone must **Forget/Remove
  the device** and re-pair, else a stale phone-side bond fails at service
  discovery with an immediate disconnect. The device auto-handles the
  reverse (phone re-pairs, device drops its stale entry).

## 3. GATT services

### 3.1 Device Information — `0x180A` (read-only)

Values are served with their **historical lengths** — strings include
the NUL terminator, the serial is space-padded to 32 bytes. Trim
trailing `\0`/spaces before comparing.

| Characteristic | UUID | Value (bytes on air) |
|---|---|---|
| Manufacturer Name | `0x2A29` | `MEATPI.COM\0` (11) |
| Model Number | `0x2A24` | `WiCAN-PRO\0` (10) |
| Serial Number | `0x2A25` | `<device_id minus its FIRST hex char>` space-padded to 32 — e.g. id `14c19f44e349` → `4c19f44e349` + 21 spaces (legacy `name+7` quirk, kept byte-exact) |
| Hardware Revision | `0x2A27` | `1_53         \0` (14) |
| Firmware Revision | `0x2A26` | `400\0` (4) |
| Software Revision | `0x2A28` | `0000\0` (5) |
| System ID | `0x2A23` | 8 × `0x00` |
| IEEE Reg. Cert. | `0x2A2A` | 8 × `0x00` |

### 3.2 Data + CLI service — `0xFFF0`

| Characteristic | UUID | Properties | Direction |
|---|---|---|---|
| **Data OUT** | `0xFFF1` | notify, indicate (read returns one dummy `0x00`) | device → app |
| **Data IN** | `0xFFF2` | write, write-without-response | app → device |
| **CLI OUT** | `0200DEC0-01EF-BC9A-5678-1234DEADF0BE` | notify, indicate (read = dummy) | device → app |
| **CLI IN** | `0300DEC0-01EF-BC9A-5678-1234DEADF0BE` | write, write-without-response | app → device |

Subscribe (write the CCCD, i.e. `setNotifyValue`/`enableNotifications`)
to Data OUT and CLI OUT before expecting anything back.

## 4. The data pipe (FFF2 → device → FFF1)

A transparent **byte stream** — no framing added by BLE. What sits on
the other end is a `bridge_manager` configuration: the **shipped
default** is `br_obd = obd <-> ble`… *when configured*; out of the box
the OBD chip is bridged to WebSocket (`ws_obd`), so pairing BLE with OBD
means setting `bridges` to `{"name":"br_ble","a":"obd","b":"ble"}` via
`/api/settings/bridge_manager` (the single-consumer rule: `obd` feeds
one bridge at a time).

With the OBD bridge active the dialect is **ELM327/STN ASCII**:

```
app writes  FFF2: "ATI\r"
app notified FFF1: "ELM327 v2.3\r\r>"      ← accumulate until '>' at line start
app writes  FFF2: "0100\r"
app notified FFF1: "41 00 FF FF FF FF \r\r>"
```

- **Notification payload cap**: `min(490, MTU − 3)` bytes. Negotiate a
  big MTU first — the device prefers **517** (Android:
  `requestMtu(517)`; iOS negotiates ~185 automatically — fine, replies
  just arrive in more notifications).
- A logical reply may span several notifications — reassemble by the
  dialect's terminator (`>` for ELM), never by packet boundaries.
- App→device writes are capped at 490 bytes per write (larger ones are
  rejected with an ATT error) — chunk to ≤ `MTU − 3` and you'll never
  hit it.

## 5. The CLI pipe (CLI IN → device → CLI OUT)

The full WiCAN command line (same registry as UART/WebSocket — commands
in `cmdline_manager/README.md`):

- Write ASCII **lines** terminated `\n` or `\r` to CLI IN (partial
  writes fine — the device reassembles, 1024-byte line max).
- The response streams as CLI OUT notifications and **ends with the
  `wican> ` prompt** — that is the "command done" marker.
- One command at a time device-wide (a mutex serializes all surfaces).
- Keep to short-running commands from BLE (status/info reads are the
  intended use). Known limitation: very long-running commands currently
  execute inline in the BLE host task and can stall the link (fix
  tracked in CHECKLIST).

## 6. Connection behavior an app MUST expect

- **WiFi hands over to you** (interface_manager, default rules ON):
  while your app is connected over BLE the device **suspends WiFi
  STA/AP** — it will drop off the LAN and its HTTP API becomes
  unreachable until you disconnect (resumes automatically a few
  seconds later). "User connects phone = user is in the car" is the
  product behavior, not a bug. It also means BLE gets a clean radio:
  measured notify throughput ~77 KB/s.
- **Connection parameters**: the device requests a window per the
  `conn_profile` setting — `ios` (default): 20–40 ms interval,
  `android_fast`: 7.5–15 ms (halves command RTT on Android; iOS
  ignores out-of-policy requests, hence the default). Measured CLI
  round trip: ~75 ms p50 on the default profile.
- **One central at a time**; advertising resumes on disconnect.
- Don't force LL data-length extension from a central that lets you;
  long LL packets destabilize the link when WiFi coex is active
  (measured — see `ble_manager_gatt_nimble.c`).

## 7. Settings that shape this interface

Via `/api/settings/ble_manager` (reboot-to-apply): `enabled` (default
false), `passkey` (default 123456), `tx_power_dbm` (default 9),
`pairing_at_boot` (default true), `bonding` (default true — §2),
`sc_only` (default true — §2), `conn_profile` (`ios` | `android_fast`).
Plus
`/api/settings/interface_manager` rules
(`sta_ble_handover`, `ap_ble_exclusive`) for the WiFi-handover behavior
in §6.

## 8. Reference clients

`tools/testbench/ble_bench.py` (pairing agent + echo + CLI),
`ble_ab.py` (device info + CLI RTT + pipes), `ble_blast.py` (throughput
counting) — bench-proven Python/bleak implementations of everything
above, including the passkey agent and bond-recovery handling.
