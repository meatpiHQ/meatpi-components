# Back-port: /api/vpn peer detail (online + direct-vs-relay) → WiCAN v6

> **APPLIED to WiCAN 2026-07-13.** One deviation from step 1 below:
> `vpn_manager_private.h` was NOT copied verbatim — ESPNetLink's copy
> also strips the `vpn_events_*` declarations (no event_manager there),
> so only the `vpn_ts_peer_t` + `vpn_ts_get_peers()` hunk was applied.
> The three `.c` files went verbatim; bench hunk added; build clean;
> WG-type `/api/vpn` shape + CLI smoke-verified on the bench DUT.
> Open: WiCAN's own `ts_bench_target.py` run (rpi001 headscale rig —
> uplink down; TODO §3).

ESPNetLink implemented the nicety WiCAN's `TASK_tailscale.md` deferred
("/api/vpn could expose peer online + direct-vs-relay"). Implemented and
bench-verified on ESPNetLink 2026-07-12 (`TS TARGET PASS` including the new
assertion; a real DISCO **direct** path was observed through LTE CGNAT).
Both repos' `vpn_manager` files were byte-identical before this change, so
the port is a straight file copy plus one bench-script hunk.

## What it adds

- `GET /api/vpn`, tailscale type only:
  `"peers":[{"hostname":"pi-bench.bench.tailnet","ip":"100.64.0.2",
  "online":true,"path":"direct"|"relay"}]` (up to microlink's 16-peer
  table). WireGuard-type responses are unchanged (no `peers` key).
- `vpn` CLI: one line per peer —
  `peer pi-bench.bench.tailnet 100.64.0.2  online  direct`.
- No new state and no polling: values come from microlink's existing
  `microlink_get_peer_count()` / `microlink_get_peer_info()` snapshot API
  (`online` from the netmap, `direct_path` from the live WG session).

## How to apply (WiCAN v6 tree)

1. **Copy these four files verbatim from the ESPNetLink repo**
   (`components/vpn_manager/`) — they were identical between the repos
   before this change, so the copies carry exactly this feature:

   - `vpn_manager_private.h` (adds `vpn_ts_peer_t` + `vpn_ts_get_peers()`)
   - `vpn_manager_ts.c` (implements the snapshot getter)
   - `vpn_manager_http.c` (GET handler rebuilt on cJSON; adds the array)
   - `vpn_manager_cli.c` (per-peer lines)

   If the WiCAN copies have drifted since 2026-07-12, apply the same four
   hunks by hand instead — each file's change is a single self-contained
   block (search for `vpn_ts_get_peers`).

2. **No build-system changes**: vpn_manager already links cJSON
   (settings use it) and microlink.

3. **Bench regression** — in WiCAN's `tools/testbench/ts_bench_target.py`,
   right after the existing `check("DUT sees the peer", ...)` add:

   ```python
   # peer detail surface: hostname/ip/online/path per peer
   pl = v2.get("peers", [])
   check("peer detail exposed (pi-bench online)",
         any(p.get("hostname", "").startswith("pi-bench") and
             p.get("online") for p in pl),
         json.dumps(pl))
   ```

## How to verify

1. Run WiCAN's `ts_bench_target.py` (headscale rig on rpi001 per
   `TASK_tailscale.md`) — expect `TS TARGET PASS` including the new
   `peer detail exposed` line. On an all-LAN bench the peer normally shows
   `"path":"direct"`; `"relay"` is equally valid (DERP) — the check only
   asserts presence + online.
2. While connected, `vpn` on the console should print the peer lines.
3. WireGuard regression: `vpn_bench_target.py` still passes (the WG-type
   JSON shape is untouched).

## Notes from the ESPNetLink run

- Headscale's sqlite accumulates node registrations across bench runs
  (every `tailscale up` registers a fresh node), so old peers appear in
  the array as `online:true, path:relay` alongside the live one. Bench
  cosmetics only — `headscale nodes delete -i <id>` or wipe the db to tidy.
- `/api/vpn` responses grow ~110 bytes per peer; the handler builds the
  body on the heap via cJSON (the old fixed 288-byte snprintf buffer is
  gone).
