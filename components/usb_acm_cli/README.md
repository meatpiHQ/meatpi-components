# usb_acm_cli

Talk to an attached USB CDC-ACM device's console over the CherryUSB **host**
stack. Overrides the WEAK `usbh_cdc_acm_run/stop` hooks, runs a
PSRAM-stacked RX task, and exposes:
- `POST /api/usb/acm/cmd` `{"cmd","timeout_ms"?}` → send one line, collect
  the reply until the device's `esp>` PROMPT (`{"ok","response",
  "connected"}`). `timeout_ms` (default 8000) is a cap, not a latency —
  collection returns the moment the prompt lands; a 700 ms quiet window
  is the prompt-less fallback. **2026-07-13 fix**: the original 150 ms
  quiet-settle raced the echo→payload gap (the modem legs of `lte`/`gps`
  go silent >1 s mid-response) and returned just the echo — the web-UI
  ESPNetLink card's "no reply" bug.
- a bridge_manager endpoint `acm` for raw passthrough.
- config `/api/settings/usb_acm_cli` (`enabled` default false, `cli`).

## GPS

A poll task runs the dongle's `gps -p -j` every 5 s (legacy usb_host
cadence) and caches the parsed fix (`usb_acm_gps.c` — a pure, no-deps,
host-tested parser: `usb_acm_gps_parse`). Two surfaces read the cache
(never any ACM I/O):

- `GET /api/gps` — the fix in device-contract field names (speed m/s).
- `usb_acm_cli_gps_get()` — for C consumers; the web UI's ESPNetLink
  Status card reads `/api/gps` rather than polling the console itself.
  When the console has no live fix it consults the **fallback provider**
  (`usb_acm_cli_set_gps_fallback()`, main wires `espnetlink_link_gps_get`)
  — the WiFi-modem topology (dongle USB data cut, reached over its AP)
  has no console at all, so `/api/gps` keeps working from the HTTP poll.
  The parser also accepts the dongle's HTTP `/api/gps` spelling
  (`latitude`/`longitude`) next to the console's `lat`/`lon`.

Each refresh also fires `usb_acm_cli_set_gps_sink()` — **main** wires it
to `autopid_publish_external()`, so a fix becomes first-class autopid
parameters (`gps_latitude`/`gps_longitude`/`gps_altitude`/`gps_speed`/
`gps_heading`/`gps_satellites`) and flows wherever autopid data goes:
the Home Assistant push (`autopid_data`), the data_logger, the
dashboard, and event rules — no GPS code in any of those. Only a LIVE
fix is published; a cached AGNSS position is never reported as current.

The console is the attached device's OWN CLI (e.g. the espnetlink dongle's
`esp>` prompt: `ver`, `lte -s/-r/-o/-i`, `gps`, `speedtest`, …), not raw AT.

**Never bound: `303A:1001`** — an ESP32-S3's ROM/bootloader USB-Serial-JTAG
is on the pads for ~1.5 s after a power-on (the ESPNetLink's app detaches
it before its real `303A:4007` composite device enumerates). CherryUSB's
class match is class-only, so `usbh_cdc_acm_run` filters that VID/PID
itself: the DTR/RTS line sequence it would otherwise send is exactly what
resets the chip (2026-08-24).

## Testing

Host suite (`host_test/`, 6 tests): the pure `usb_acm_gps_parse` — the
`gps -p -j` → fix mapping incl. the `"lat"` vs `"cached_lat"` quote
disambiguation and the "valid:false → not a fix, cached position not
adopted" rule. The rest of the component is inseparable from live USB
host I/O + DMA (no pure unit on the `linux` target); it's covered on the
bench — driven end-to-end against the espnetlink dongle's console
(`ESPNETLINK TARGET PASS`, `tools/testbench/espnetlink_bench.py`): the
RX-settle collector, the HTTP/bridge paths, and the GPS poll → autopid
publish path run on real hardware.
