# espnetlink_link

The ESPNetLink LTE/GPS dongle as the WiCAN's internet uplink — **zero-touch
pairing over USB**, then either the **WiFi-modem** topology (default: the USB
cable carries 5 V only, the dongle is reached over its own WiFi AP) or plain
**USB-Ethernet** (`usb_ncm` / `usb_rndis`: the dongle stays a USB-Ethernet
adapter — CDC-NCM or RNDIS — on the `usb_host_manager` uplink; the pairing
pass sets the dongle's `usb_dev_ethernet.class` to match). Feature layer; sits above `wifi_manager`,
`usb_host_manager`/`usb_eth_host`, `http_client_manager`, `settings_manager`.

Why two modes: the WiCAN's USB host transmitter desenses the dongle's GNSS
while the USB-Ethernet link is active — a cold fix never completes
(espnetlink-fw `docs/espnetlink_wifi_mode_howto.md`). With the data lines
cut at the dongle's mux and the WiCAN joined to the dongle's AP, internet
still flows (the dongle NATs its AP into LTE) and GPS rides `GET /api/gps`.

Contract: espnetlink-fw `docs/espnetlink_wican_integration.md` (dongle
branch `refactor`, `components/wifi_modem/`). Dongle identity on USB:
**VID `0x303A` PID `0x4007`**; its address on the NCM link `192.168.7.1`
(the WiCAN gets `192.168.7.2/24` by DHCP — address + netmask only, no
router unless `ncm_share`); its AP `ESPNetLink_<id6>` at `192.168.80.1`.

## The zero-touch sequence (`mode=wifi_modem`, `auto_pair=true`)

```
dongle power-on → NCM + CDC enumerate (~3 s)
WiCAN: NCM up + IP, VID/PID 303A:4007          (usb_host_manager status, 1 s tick)
  → GET http://192.168.7.1/api/info             device_type=espnetlink, device_id
  → GET /api/wifi_modem/credentials             {ssid,password,device_id} — USB only (403 over WiFi)
  → store: wifi_manager fallbackN (trusted) + espnetlink.ssid/device_id   CHANGE-GUARDED
  → POST /api/wifi_modem/usb_data {"enabled":false}   200 → the dongle cuts its data lines 500 ms later
  → the NCM device drops ("usb gone: cable is power only")
  → key was new/changed → the WiCAN reboots ONCE (reboot-to-apply); unchanged → nothing
later: wifi_manager joins ESPNetLink_<id6> as a fallback candidate (home network stays primary)
  → uplink = espnetlink (AP) → GET /api/gps every gps_poll_s, GET /api/wifi_modem every health_poll_s
```

Bench (WiCAN Pro + ESPNetLink v1.22-39, 2026-08-24): link-up → cut done
**1.6–4.7 s**, power-on → cut **~15 s**, AP joined ~5 s after the cut, LTE
health ~30 s, GPS fix served over the AP (13–15 sats). `espnetlink repair`
(VBUS cycle) ×5 and PSU cold cycle ×2: every cycle cut, 0 WiCAN reboots
(key unchanged), 0 error lines, 0 panics.

Rules (contract §3) as implemented by the pure machine in
`espnetlink_link_core.c` (`espnl_sm_step`, host-tested):

| State | Meaning / exits |
|---|---|
| `idle` | no ESPNetLink on USB. `LINK_UP` (303A:4007) → `identify`; any other NCM device → `foreign` |
| `identify` | `/api/info` (retried every tick for 5 s) → `read_key` (`usb_ncm`: → `ncm_share`); not an ESPNetLink / timeout → `foreign` |
| `read_key` | credentials → store → `cut` (POST). 5 s without a key → VBUS recovery |
| `cut` | 200 → `wait_drop`; **409** (dongle `usb_mode=ncm`, the owner locked USB on) → `tether`; fail → re-POST up to `cut_retries`, then recovery |
| `wait_drop` | the device must disappear within 3 s (one more POST at 3 s — the dongle answers "already"); still there at 10 s → recovery |
| `done` | cut landed (cable = power only). `REBOOT` if the key changed. Re-enumeration → `identify` again (a swapped/reset dongle self-heals); `AP_STALE` → recovery |
| `tether` | leave the USB link alone; the stored key still works for the AP |
| `foreign` | ignore until the device re-plugs |
| `ncm_share` / `ncm_up` | `usb_ncm` mode only — see below |

**Boot-cut steady state (2026-08-24).** The first cut arms the dongle's
`wifi_modem.boot_cut` hint: every later dongle boot cuts its data lines
at ~3 s, before its USB stack starts — this component then sees NO USB
device at all (`pair_state=idle` while paired + on the AP is the normal
steady state), and GPS acquires from the dongle's power-on with a quiet
bus. The dongle self-restores its USB after 60 s without an AP client (a
PC gets tethering back; a stale-key WiCAN gets its read channel), so
VBUS-cycle recovery is ~75 s worst case. The WiCAN also no longer
VBUS-power-cycles at host start (usb_eth_host 2026-08-24): a WiCAN
reboot never reboots the dongle or costs its fix.

Recovery = `usb_host_manager_set_vbus(false)` 1 s `true` (the dongle
re-boots and re-enumerates), spaced ≥ 60 s and at most 3 per unbroken
failure run, then it gives up until the next re-plug (`GIVE_UP`). The
operator's `repair` always spends one cycle. **AP stale** = paired + dongle
present (ID pin) + cut + STA not suspended by policy, yet the STA sits on
NO network for 180 s → the stored key is presumed rotated → one cycle →
the key is re-read. A VBUS cycle (or 3 consecutive failed polls on a
"connected" STA) also **re-joins the AP**: the dongle's AP reboots faster
than the driver's beacon-loss window, so the STA keeps a zombie
association the dongle has forgotten (`wifi_manager_sta_reconnect()`,
bench 2026-08-24) — polls are held 15 s meanwhile.

## Fresh devices (out of the box)

**Pairing hold (2026-09-07, reworked 2026-09-08):** `wifi_manager`
refuses any settings save that keeps the factory AP password, and the
zero-touch key store is such a save. The machine still identifies the
dongle and reads its key (so the status shows the dongle's id, firmware
and API level), then parks in the `hold` state instead of storing or
cutting: no retries, no VBUS cycles. `pair_blocked_factory_pw` (a
boot-time fact for an unpaired device) and `last_error` say why, the web
UI shows the note on the ESPNetLink card, the CLI prints the state and
the text. A PAIRED WiCAN that meets a re-provisioned dongle (new key)
while its AP still has the factory password parks the same way — the
gate refuses the changed key and `last_error` carries the gate's own
message. Changing the AP password reboots the device (reboot-to-apply)
and the next enumeration pairs by itself.

**Unsupported dongle firmware (2026-09-08):** a dongle that answers
`/api/info` but 404s the WiFi-modem routes parks in `unsupported` —
the bench found a July test build (`v1.22-41-gf0e8804`, api 6) on a
field unit: no credentials route, no `/api/wifi_modem` health, no
`usb_dev_ethernet.class`, LTE never started, AP named `GPS-USB-TEST`.
It used to surface as "Not an ESPNetLink / gave up" plus VBUS churn;
now `last_error` names the missing route and says to update the dongle
firmware, the health poll sets `health_unsupported` instead of leaving
the LTE panel on "waiting for the first poll", and in the USB modes a
missing settings API keeps the link (`ncm_up`) with `last_error`
explaining why the class stayed CDC-NCM. `/api/info`'s `fw_version` /
`api_level` live in the status (`dongle_fw`, `dongle_api`,
`dongle_api_min` = `ESPNL_MIN_API_LEVEL`, 7 = the WiFi-modem surface);
a level below the minimum is flagged in the UI even when the routes
happen to exist.


A fresh WiCAN boots with `wifi_manager.mode=ap` and no STA networks; the
pairing store flips it to `apsta` and the one reboot after the cut
applies it. The user is typically joined to the WiCAN's AP with a phone
while this happens — since 2026-08-31 `wifi_manager` lets the first STA
association of a boot through despite that client (it used to pause
indefinitely while anyone sat on the AP, so a fresh device never joined
the dongle — field-hit). A fresh dongle provisions its per-device AP
password on first boot (one dongle reboot before USB starts), so the key
the WiCAN reads is always the final one. Bench:
`tools/testbench/wifi/espnetlink_fresh_bench.py` (erase + flash both,
cold boot, Pi as the user's phone on the WiCAN AP).

## `mode=usb_ncm` / `mode=usb_rndis` (the data lines stay on)

The dongle is a USB-Ethernet uplink exactly like before (`usb_host_manager`
owns the netif; `prefer_usb_route` decides the default route when WiFi is
also up; the host attaches whichever class enumerates — `cdc_ncm` or
`rndis` driver). The machine makes sure the dongle side matches: it reads
`lte_upstream_pppos.ncm_share` (**shares its LTE link over USB** — DHCP
router + DNS + NAPT) and `usb_dev_ethernet.class` (`ncm`/`rndis` per our
mode; 404 = pre-2026-08-25 dongle firmware, logged and left on its
built-in class), PUTs whichever differs and `POST /api/settings/submit`s
ONCE (the dongle reboots once to apply). GPS and health are polled over
the USB link at `192.168.7.1`. Switching here from `wifi_modem` finds the
dongle's data lines cut (boot-cut re-cuts them on every dongle boot, and
with the WiCAN on its AP the dongle's 60 s no-station fallback never
fires) — the link task then restores them over the AP: dongle health
`usb_data=0` -> `POST /api/wifi_modem/usb_data {"enabled":true}` (clears
the boot-cut hint; retried every 15 s), USB enumerates, the machine takes
over. Note the dongle keeps `ncm_share=true` and
the class after switching back to `wifi_modem` (a PC on that dongle would
then tether through LTE over that class) — change them on the dongle if
that matters.

## API

| Function | One-liner |
|---|---|
| `espnetlink_link_init()` | Log + settings descriptor, store worker semaphores. No network. |
| `espnetlink_link_start()` | Start the link task (1 s tick) when `enabled`; after `wifi_manager` + `usb_host_manager`. `ESP_ERR_INVALID_STATE` if unconfigured. |
| `espnetlink_link_stop()` | Stop the task on its next lap. |
| `espnetlink_link_set_gps_sink(sink)` | ONE sink for parsed fixes (same signature as `usb_acm_cli`'s — main wires both to autopid). |
| `espnetlink_link_gps_get(out)` | Cached fix; `ESP_ERR_NOT_FOUND` without a live fix — doubles as `usb_acm_cli`'s fallback provider for `/api/gps`. |
| `espnetlink_link_status(out)` | `espnetlink_link_status_t`: mode, pairing state, uplink, dongle health, counters. |
| `espnetlink_link_pair(ssid, password, &slot)` | Manual pairing into a wifi_manager fallback slot (+ our ssid/enabled); reboot-to-apply. |
| `espnetlink_link_repair()` | VBUS cycle → re-enumeration → key re-read. `ESP_ERR_INVALID_STATE` when the USB host is down. |
| `espnetlink_link_register_http()` | `GET /api/espnetlink`, `POST /api/espnetlink/pair`, `POST /api/espnetlink/repair` (`components/HTTP_API.md` §6e9b). |
| `espnetlink_link_register_cli()` | `espnetlink [pair <ssid> <password> \| repair]` — self-registered on the settings apply (`cli`). |

Files: `espnetlink_link.c` engine/polls · `espnetlink_link_usb.c` the USB
side (machine driver, HTTP over USB, store, VBUS, reboot) ·
`espnetlink_link_pair.c` the change-guarded settings store ·
`espnetlink_link_core.c` pure parsers + machine · `espnetlink_link_migrate.c`
pure migration · `_settings.c` / `_http.c` / `_cli.c`.

## Dependencies

`usb_acm_cli` (public: the fix struct/parser/sink type), private:
`settings_manager`, `log_manager`, `wifi_manager` (`is_sta_connected`,
`mode_has_sta`, `sta_reconnect`), `usb_host_manager` (status incl.
VID/PID, `set_vbus`), `http_client_manager`, `http_server_manager`,
`api_http` (`note_settings_changed`), `restart_tracker`
(`SOURCE_PAIRING`), `dev_status_manager` (STA-suspended bit),
`cmdline_manager`, `espressif/cjson`. Init after those; start after
`wifi_manager_start` and `usb_host_manager_start`. Main glue:
`main_glue_wire_gps()` wires the sink + the `/api/gps` fallback.

Additions this component needed elsewhere (2026-08-24): `usb_eth_host`
remembers the enumerated device's VID/PID (`usb_eth_host_get_active_device_ids`)
and fires `on_eth_ip_lost` from its driver-stopped path (lwIP posts no
`IP_EVENT_ETH_LOST_IP` for a stopped+destroyed netif — the cut looked like
"uplink still up"); `usb_host_manager` exposes `vid`/`pid`/`vbus_on` in its
status and `usb_host_manager_set_vbus()`; `usb_acm_cli` ignores `303A:1001`
(the S3 boot-window serial-JTAG: its DTR/RTS toggle resets the dongle) and
gained the GPS fallback provider; `wifi_manager_sta_reconnect()`.

## Settings (`"espnetlink"`, version 2)

| Key | Type / default | Notes |
|---|---|---|
| `enabled` | bool `true` | the link task runs (v1 default was false) |
| `mode` | `wifi_modem` \| `usb_ncm` \| `usb_rndis`, `wifi_modem` | see above |
| `auto_pair` | bool `true` | zero-touch over USB; false = manual `pair` only, USB link left alone |
| `ssid` | str ≤32 `""` | the dongle's AP (written by pairing) |
| `device_id` | str ≤12 `""` | the paired dongle (written by pairing) |
| `host` | str ≤15 `""` | dongle address override; "" = the STA gateway on the AP / `192.168.7.1` on USB |
| `gps_poll_s` | 1..60 `2` | |
| `health_poll_s` | 5..300 `10` | |
| `cut_retries` | 1..5 `2` | POST `usb_data` attempts before a VBUS recovery |
| `cli` | bool `true` | |

Migration v1 → v2 (`espnl_settings_migrate`, host-tested): a v1 object
that never paired (`ssid` empty) gets `enabled=true` (it only carried v1's
old default); a paired one keeps its choice; new keys come from the schema.
Pairing also edits `wifi_manager`: `fallbackN_ssid/password/trusted` and
`mode` `ap`→`apsta` / `off`→`sta` (the dongle is reached over STA) — every
write change-guarded, so the per-boot key re-read costs **0 flash writes**
and no reboot when nothing changed (§11). Recommended for the car:
`wifi_manager.sta_roam_interval_s=60` (how often it looks for the home
network while parked on the dongle); `interface_manager.sta_ble_handover`
still suspends the STA while a BLE client is connected (dongle uplink
drops — phase-2 exemption not built).

## Memory footprint (measured 2026-08-24 unless noted)

- Task `espnl_link`: 6144 B stack in **PSRAM** (`EXT_RAM_BSS_ATTR`), prio 3;
  plain-HTTP client only (no TLS), never writes flash itself.
- Store worker `espnl_store`: 6144 B **internal** (default heap), one-shot,
  exists only during a pairing store — `settings_manager_set()` walks
  LittleFS on the caller's stack with the cache disabled (the §2 corollary;
  the first bench attempt panicked from the PSRAM stack).
- Reboot task `espnl_reboot`: 3072 B internal, one-shot, once per boot at most.
- Static: ~1.2 KB `.bss` (status, health, fix, machine) — estimated.
- Heap: one `http_client_manager` response (≤ 1–2 KB PSRAM) per poll, freed.
- Flash: 0 writes per boot when the key is unchanged (bench: `WICAN FLASH
  writes=0`); a new key = 2 settings objects once.

## Testing

Host suite (`host_test/`, **26 tests**): identity, SSID match, the
`/api/wifi_modem` / `/api/info` / credentials parsers (escapes, 403 body,
oversize), the changed-key compare, URL build, the shared GPS parser with
the HTTP document shape, the v1→v2 migration, and the pairing machine
(happy cut, changed key → reboot after the drop, drop racing the POST,
409 tether, foreign device, identify retry budget, cut retry → recovery,
drop timeout → re-POST → recovery, recovery budget + spacing + give-up,
re-enumeration re-reads the key, AP-stale + operator re-pair, `usb_ncm`
share flow, init clamps). Run:
`tools/run_host_tests.sh espnetlink_link` (CI) or on the bench Pi
`~/wican/tools/testbench/run_host_tests.sh espnetlink_link`.

Bench (the contract's §5 plan, WiCAN COM1175 + dongle COM1176 + PSU):
boot log `identified … → credentials ok → usb_data off requested → usb
gone → STA got IP 192.168.80.x → uplink: none -> espnetlink (AP) → dongle
LTE up`, `espnetlink` → `gps: valid=1`, `rtc -s` syncs SNTP through the
dongle, `espnetlink repair` ×N cuts every time with 0 reboots / 0 `E`
lines, `mode=usb_ncm` → `dongle ncm_share turned on` → dongle reboots →
`dongle shares its LTE link over USB` → `uplink: espnetlink (USB)`, GPS +
SNTP over USB. Cold GPS TTFF (PSU power cut, the
dongle's `/api/gps` polled at 1 Hz by an independent AP client, A/B ×3
alternated): **55/65/62 s with the WiCAN's WiFi active** (STA joined +
2 s polls) vs 72/84/102 s with the WiCAN's radio stopped — no
WiFi-proximity regression, and the normal flow's median (62 s) matches
the ESPNetLink campaign's power-only envelope (45–121 s, ~50% fix
availability, untuned-antenna sample — espnetlink-fw `gps-test-sandbox`
`CH343_ROUTE_RESULTS.md`). The integration cuts USB at ~15 s, well
before acquisition matters.
