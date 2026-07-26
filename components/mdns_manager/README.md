# mdns_manager — mDNS advertisement (feature)

Rewrite of the legacy `wc_mdns.c`, preserving the ON-AIR CONTRACT the
**Home Assistant integration** discovers by:

| Field | Value (legacy-exact) |
|---|---|
| hostname | `wican_<device_id>` → `wican_<id>.local` |
| instance | `wican web server` |
| service | `WiCAN-WebServer`, type **`_wican._tcp`, port 80** |
| TXT `mac` | STA MAC, colon-separated UPPERCASE (HA's stable unique ID — note: the STA MAC, not the AP-MAC device id) |
| TXT `device_id` | the 12-hex device id |
| TXT `device_type` | `CONFIG_WICAN_DEVICE_TYPE` (contract-v2 profile slug, default "wican_pro") |
| TXT `firmware` / `version` | the running app version (legacy Pro shipped these keys with EMPTY values — same keys, real data now) |
| TXT `hardware` | `CONFIG_WICAN_HW_VERSION` (Kconfig, default "WiCAN-PRO") |
| TXT `path` | `/` |

**Device-contract v2 (2026-07-11)**: a second service of type
**`_meatpi._tcp`, port 80** (same instance name) is advertised in
parallel — the brand-wide discovery surface required by the HA
integration 3.0 (`ha_webhooks/device-contract/ADDING_A_DEVICE.md` A1;
integration 3.0 matches only `_meatpi`/`_wican` service types — the
legacy `_http._tcp` instance-name matching was removed). Its TXT set is
the contract-v2 one: `device_type`, `device_id`, `mac`, `fw` (app
version), `api` (`"6"`).

**Live-verified 2026-07-04** from rpi001 (avahi): service resolved with
all six TXT records, `wican_14c19f44e349.local` → the device IP. Pure
string builders (mac/hostname formats) host-tested byte-exact (3/3).
(`_meatpi._tcp` + `device_type` added 2026-07-11 — re-verify on the
next bench pass.)

## API

| Call | Behavior |
|---|---|
| `mdns_manager_init()` | Settings + log descriptors. |
| `mdns_manager_start()` | Responder up + service advertised. esp-mdns tracks interface events itself, so this is safe before the network exists. No-op when disabled. |
| `mdns_manager_stop()` | Withdraw + free. |
| `mdns_manager_hostname()` | `wican_<id>.local` (the legacy getter). |

## Settings (`"mdns_manager"`, version 1, field table)

`enabled` (bool, true).

esp-mdns is a **managed component** (`espressif/mdns`, un-bundled from
IDF). No HTTP surface — mDNS is its own protocol.

## Memory

esp-mdns task + buffers (its own allocs, ~4 KB); component state a few
dozen bytes. (estimated)
