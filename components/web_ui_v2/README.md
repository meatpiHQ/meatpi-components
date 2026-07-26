# web_ui_v2

The **v2** built-in web app — a self-contained, responsive single-page
application that covers every `/api` + `/ws` feature. Like v6 it is
**API-first**: just another client, no privileged paths.

## Choosing the UI at build time

`Kconfig` adds a choice (menuconfig → *WiCAN built-in web UI*):

| Option | What compiles |
|---|---|
| **v2** (default) | this component — one gzipped `index.html` |
| **v6** | `components/web_ui` — the legacy ES-module pages |
| **None** | headless: `/api` + `/ws` only, no UI blob in flash |

Only the selected UI is embedded; `main` calls both `web_ui_v2_register()`
and `web_ui_register()` but the unselected one is a no-op. API/WS routes
always win because the catch-all is installed last.

Set it with `idf.py menuconfig` or in `sdkconfig.defaults`:
```
CONFIG_WICAN_WEBUI_V2=y     # or CONFIG_WICAN_WEBUI_V6=y / CONFIG_WICAN_WEBUI_NONE=y
```

## Design — "WiCAN Pro" (2026-07-10)

The visual system and information architecture come from **meatpi's
claude.ai/design project "WiCAN Pro configuration interface"**
(`WiCAN Pro.dc.html`, imported via the design MCP) and are implemented
over the existing SPA engine. One file, zero dependencies (no framework,
no CDN — the JetBrains Mono stack falls back to system monospace). It is
gzipped at build time (`gzip_asset.py`) and embedded (~39 KB gz).

- **Shell**: 58 px header — meatpi WiCAN Pro logo · connection pill
  (pulse dot, device id, IP, AP/STA tag) · theme toggle · a header
  **Submit Changes** button that batches settings PUTs + submit (the
  old apply-bar semantics, incl. the unsaved-changes guard).
- **Nav**: DEVICE group (the designed 12 tabs) + TOOLS group (the
  deeper diagnostic pages); a warning card at the bottom surfaces
  degraded / restart-pending components. Wraps to a chip row ≤ 860 px.
- **Connection watchdog**: 3 s `/api/status` poll drives the pill,
  offline/reboot banners, and page re-render on reconnect.
- **Theme**: light-default per the design, full dark theme, follows the
  OS and a manual toggle (persisted).
- **Schema-driven settings**: forms are generated from
  `GET /api/settings/<name>/schema`, grouped into the design's cards —
  new firmware fields appear with no UI change.

## Pages

DEVICE: Status (stat cards + network/system tables) · Settings (WiFi/AP
· Station & Bluetooth w/ scan · CAN · MQTT) · Automate (autopid
behaviour · Home Assistant webhook · destinations-as-event-rules · PID
table/scan/vehicle-profile/raw-config) · Power Saving · Logger (status +
gate + all stream/format settings) · Dashboard (live autopid gauges) ·
**CAN Monitor** (grouped IDs w/ hot-byte highlight · trace · transmit
incl. cyclic — speaks **slcan over `/ws/can`**; bridge `ws_can ↔ can`
with the slcan translator to feed it) · Terminal (console `/ws/cli` or
ELM327 `/ws/obd`) · Advanced (IMU · OBD chip · USB · radio arbitration)
· System (reboot · backup/restore · restart history · factory reset ·
**Certificates** (cert_manager sets: list w/ part flags, per-part PEM
upload via raw `/api/certs/upload?set&type` — NOT the api() JSON
wrapper — delete w/ confirm; key material is write-only by design; the
MQTT and Home Assistant `cert_set` fields become dropdowns fed from
`/api/certs` via `certSetPicker()`, which MUST share the settingsForm's
staged values object) · OTA with progress) · VPN (settings + keygen +
debug) · **USB** (connector role/uplink status · **ESPNetLink Status**
= legacy-usb_host-tab parity: LTE + GPS panels + full-JSON `<details>`,
polled from the dongle's own `lte -j` / `gps -p -j` console over
`/api/usb/acm/cmd` at the legacy cadences [GPS 5 s, LTE 15 s]; falls
back to the cached AGNSS position when there's no live fix · ESPNetLink
Console for manual commands, serialized against the poll via a shared
`nlBusy` latch so they never 409 each other) · About.
TOOLS: Trouble Codes (scan/describe/clear + databases) · DBC Signals ·
Rules & Events · Scripts · UDS Tool · J2534 · Files · Logs · System
Monitor (tasks/CPU/heap/temp) · All Settings (every component).

## Connections card (Settings page — the bridge builder)

Pure settings composition, ZERO firmware surface: each row is
[interface ▾] ⇄ [connection ▾] with contextual fields (TCP/UDP → port,
WebSocket → path, CAN → protocol raw/slcan/gvret/realdash) + enabled +
Add (max 4). Compiles into `bridge_manager.bridges` AND maintains the
backing `socket_manager.servers` (reuse by proto+port, else allocate a
free/disabled slot, name `tcp<port>`) / `websocket_manager.channels`
(reuse by path, name `ws_<path>`). Client-side guards: single-consumer rule, port and
`/ws/` path validation, slot caps, "CAN bus disabled" hint. Bridges it
can't model (hand-made relay pairings) show read-only with a pointer to
All Settings. Everything applies through the header Submit
(reboot-to-apply).

## Preview + tests WITHOUT a device — `tools/webui_preview/` (2026-07-13)

The committed harness mocks the whole `/api` + `/ws` surface in-page:

```
cd tools/webui_preview
python make_preview.py     # extracts all 34 components' settings field
                           # tables from the C sources into mocks, and
                           # injects mock_api.js into index.html
start preview.html         # clickable in any browser, zero device
npm i jsdom                # once
node smoke.mjs             # every route, assert 0 JS errors + non-empty view
node probe_automate.mjs    # interaction probe: Automate + Rules editor flows
node probe_adv.mjs         # advanced-gating visibility probe
```

Last run (2026-07-13): smoke 23 routes 0 errors; automate probe 16/16
(card order, essentials-only gating, scan→results-modal→dedup-merge,
rule-editor modal). The preview is also publishable as a claude.ai
artifact for review. Against a REAL device instead:
`cd components/web_ui_v2/web && python -m http.server 8000` then
`http://localhost:8000/?api=http://<dut-ip>`.
