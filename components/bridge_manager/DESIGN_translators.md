# CAN Translators — Design & Implementation (SLCAN · GVRET · RealDash)

> Implements the three deferred CAN framing codecs as `bridge_translator_t`s
> plus the missing **internal-CAN endpoint** they all bridge to. Contract:
> `bridge_translator_t` in `bridge_manager/include/bridge_manager.h`. Ports the
> protocol logic from `components/legacy/{slcan,gvret,realdash}.{c,h}` into the
> pure/testable v6 translator model. Companion TASK specs:
> `translator_slcan/TASK_*`, `translator_gvret/TASK_*`.

---

## 1. How the pieces fit

```
   CAN bus ── can_manager ──[ "can" ENDPOINT ]──┐
                                                 │  bridge_manager pump
   client (PCAN/SavvyCAN/RealDash) ── socket ──[ "slcan0"/"gvret0"/"rd0" ENDPOINT ]
                                                 │
                              TRANSLATOR (slcan | gvret | realdash)
```

- A **bridge** is settings config `{name, a, b, translator, enabled}`. Example:
  `{"name":"br_slcan","a":"can","b":"slcan0","translator":"slcan"}`.
- The pump runs **a→b through `decode`, b→a through `encode`** (contract §"Direction
  convention"). So the codecs are written for **`a` = the CAN endpoint,
  `b` = the client socket**:
  - `decode` : CAN-wire chunk (from `can`) → client bytes (to socket)
  - `encode` : client bytes (from socket) → CAN-wire chunk (to `can`)
  - **Config rule (must hold): the CAN endpoint is always side `a`.** Documented
    in each translator README + validated where practical.
- Endpoints already exist as socket/glue: `slcan0`, `gvret0` (TCP, from
  socket_manager), plus we add `rd0` for RealDash. The **`can` endpoint is new**
  (this design) — the "internal-CAN endpoint" both TASK specs waited on.

Why this model: the codecs stay **pure** (ctx-only state, sink-only output,
host-testable with no FreeRTOS/driver), exactly as the contract and the
`test_translator_contract.c` splitter demonstrate. All CAN I/O lives in the one
`can` endpoint; all socket I/O in socket_manager. New protocol = new pure codec.

---

## 2. CAN-frame wire format (the chunk the `can` endpoint speaks)

Both the `can` endpoint and every translator serialize a CAN frame into a
`bridge_chunk_t` payload with this fixed little-endian layout. Defined once in a
dependency-free header **`can_manager/include/can_frame_wire.h`** with
`static inline` pack/unpack so the endpoint glue and the translators share exactly
one definition (no drift).

```
offset  size  field
  0      4    identifier (uint32 LE; 11- or 29-bit)
  4      1    flags: bit0 = extended, bit1 = RTR   (other bits reserved 0)
  5      1    dlc (0..8)
  6      N    data[dlc]      (absent for RTR / dlc 0)
             total length = 6 + dlc  (min 6, max 14)
```

API (inline, pure):
- `size_t can_wire_encode(const can_core_frame_t *f, uint8_t *out)` → bytes
  written (6 + dlc). `out` must hold ≥ 14.
- `bool can_wire_decode(const uint8_t *in, size_t len, can_core_frame_t *f)`
  → false if `len < 6` or `len < 6 + dlc` or `dlc > 8`.

One chunk = one frame. The pump never splits a frame across chunks for the `can`
endpoint (frames are ≤14 B ≪ 128 B chunk), so the CAN side needs no reassembly;
only the **client side** is a fragmented stream (handled in each codec's ctx).

Rationale for LE id + flags byte (vs j2534's `[4B BE id][data]`): translators
need ext/rtr, and SavvyCAN/GVRET already use LE ids — LE keeps the gvret path a
near-memcpy. This format is bridge-internal (never leaves the device), so it is
free to differ from the j2534 wire.

---

## 3. The `can` endpoint (new glue)

A `bridge_endpoint_t` named `"can"`, registered in the adapter glue —
`main/main_endpoints.c` when this was designed, `components/
bridge_endpoints` since the extraction (matches how `obd`/`slcan0` are
glued; providers don't depend on bridge_manager).

- **`send(data, len)`** — `can_wire_decode(data,len,&f)`; on success
  `can_manager_send(f.id, f.ext, f.rtr, f.data, f.dlc)`. Malformed → count + drop.
- **`subscribe(q)`** — `can_manager_subscribe_queue(q_internal, 0, 0, false,
  monitor_all=true, &idx)` to get every frame, then run a small **pump task**
  that blocks on `q_internal` (can_core_frame_t), `can_wire_encode`s each into a
  `bridge_chunk_t`, and `xQueueSend`s to the bridge's `q`. (The bridge pump reads
  `q` as the endpoint's RX.) One subscriber at a time is enough for v1 (a bridge
  owns the endpoint); guard against a second subscribe.
- **`unsubscribe(q)`** — stop the pump task, `can_manager_unsubscribe_queue(idx)`.
- Internal-RAM note: the pump task stack goes to PSRAM (`EXT_RAM_BSS_ATTR`), TCB
  internal; it never touches flash. The encode scratch is stack-local (14 B).

Bus config (bitrate/silent/enable) is **can_manager's** settings job, NOT the
codec's — SLCAN `S`/`O`/`C` and GVRET `SETUP_CANBUS` are **absorbed** (see §4/§5).
The bus must be enabled in `can_manager` settings for a bridge to carry traffic;
document this in each README ("enable CAN bus first").

---

## 4. translator_slcan (Lawicel ASCII)  — testable now (python-can + PCAN)

Port of `legacy/slcan.c`, stripped to a pure codec. New component
`components/translator_slcan/` registering itself in `translator_slcan_init()`
via `bridge_manager_register_translator()`.

### decode — CAN-wire chunk → slcan ASCII (`tIIIL DD..\r` / `TIIIIIIIIL DD..\r`)
Port of `slcan_parse_frame()`:
- `can_wire_decode` the chunk. Emit `t`(11-bit data) / `T`(29-bit data) /
  `r`(11-bit RTR) / `R`(29-bit RTR); id as 3 or 8 upper-hex nibbles; one hex
  DLC digit; `dlc*2` hex data nibbles; trailing `\r`. Timestamp suffix
  supported iff the `Z1` command enabled it (ctx flag). One chunk → one `sink`
  call (one line). ~30 B max (`SLCAN_MTU`).

### encode — slcan ASCII stream → CAN-wire chunk(s)
Port of `slcan_parse_str()` + `slcan_set_frame()`, made pure & reentrant on ctx:
- Byte-fed state machine reassembling commands across chunk boundaries in the
  **ctx** (no statics — the legacy used file-scope statics; move them all into
  `slcan_ctx_t`). On a complete `t/T/r/R` line → build `can_core_frame_t` →
  `can_wire_encode` → `sink` (one chunk = one frame). Many frames per input chunk
  supported (loop). Malformed / overflow → reset the line, drop, no ctx
  corruption.
- **Control commands are absorbed, not answered** (v1): `O`/`C`/`L`/`Y` (open/
  close/listen/loopback), `S<n>`/`s...` (bitrate), `Z0/Z1` (timestamp → ctx
  flag, the one that affects decode), `M`/`m` (filter/mask), `F`/`V`/`N`
  (status/version/serial). They consume their line and emit nothing. Rationale:
  the bus is configured by `can_manager` settings; **python-can's `slcan`
  interface only *writes* `O`/`S`/`C` and never requires a reply**, and its RX
  path only parses `t/T/r/R` — so absorb-control + round-trip-frames is exactly
  what it needs. (If a client is found that needs `V`/`N`/`F` replies, use the
  reply channel from §6 — but slcan v1 does not.)

`ctx` (≤256 B): line-reassembly buffer + parse state (header/body/end, cmd,
frame-build index/id/data), `timestamp_flag`, a 100 ms inter-byte reset timer is
DROPPED (host-pure; the pump delivers contiguously enough — instead reset on any
framing error).

### Tests
- Host (`host_test/`): encode round-trips for 11-bit / 29-bit / RTR / dlc 0..8;
  decode formats each back; **re-split the same input at every byte boundary**
  (fragmentation) → identical frames; one chunk carrying several `t` lines →
  several frames; malformed (`t` with short id, bad hex, over-long) rejected with
  clean ctx; `Z1` then a frame → timestamped decode.
- Bench (python-can + PCAN, per meatpi): a `raw`-vs-`slcan` bridge on
  `tcp0:3333`; `python-can` `slcan` bus connects, `bus.send()` a frame → PCAN
  sees it; PCAN injects → python-can `recv()` sees it. Script:
  `tools/testbench/slcan_bridge_test.py`.

---

## 5. translator_gvret (SavvyCAN binary)  — needs the reply channel (§6)

Port of `legacy/gvret.c`. New component `components/translator_gvret/`.

### decode — CAN-wire chunk → GVRET frame record
Port of `gvret_parse_can_frame()`: `F1 00 <ts:4 LE> <id:4 LE, bit31=ext> <dlc:1>
<data:dlc> <checksum:1>` (XOR checksum). Timestamp = a ctx microsecond counter
(monotonic; seeded at ctx_init, advanced by the pump is not available — use a
free-running value derived from a ctx counter; SavvyCAN tolerates relative ts).
One chunk → one record.

### encode — GVRET command stream → CAN-wire TX frames **+ protocol replies**
Port of `gvret_parse()` state machine into the ctx. Handles:
- `F1 00 …` BUILD_CAN_FRAME → assemble frame → `can_wire_encode` → `sink` (far).
- `F1 01` TIME_SYNC, `F1 06` GET_CANBUS_PARAMS, `F1 07` GET_DEV_INFO,
  `F1 09` KEEPALIVE, `F1 0C` GET_NUMBUSES, `F1 0D` GET_EXT_BUSES, `E7` binary
  mode — these **reply to the client** via the **reply channel** (§6). Values:
  build num `CFG_BUILD_NUM`, single bus, speed from an injected getter (see §6).
- `F1 04/05/08/0A/0B/0E` (dig-out, setup-canbus, sw-mode, systype, echo,
  ext-buses) → parse+absorb (setup-canbus does NOT reconfigure the bus in v1;
  bus is can_manager's; log + ignore, or optionally forward bitrate — decide
  with meatpi; default: absorb).

### Reference fixture (the "figure it out" part)
GVRET dialect: capture a real **SavvyCAN ↔ legacy-WiCAN (TCP:23)** session with
Wireshark and save as `translator_gvret/fixtures/savvycan_session.bin` — the
host test replays it (whole + re-split at every boundary) and asserts the exact
reply bytes. Until we have that capture, the host test uses hand-built vectors
from the legacy code's constants; the fixture upgrades confidence. Bench: SavvyCAN
connects through a `can↔gvret↔tcp` bridge, sees injected PCAN traffic, and its TX
appears on PCAN.

`ctx` (≤256 B): parse state + step + build ints + a small reply-assembly area +
the §6 reply header + an injected device-info getter.

---

## 6. Reply channel — a minimal, backward-compatible bridge_manager extension

GVRET (and RealDash 0x66 request/response, if used) must answer the client on
the **same side** the command arrived. The pure `decode`/`encode` sink only
reaches the *far* endpoint. Add a clean near-side reply path **without changing
existing signatures** (so `raw` and the test codec are untouched):

- The first bytes of a reply-capable translator's `ctx` are a standard header:
  ```
  typedef struct { bridge_sink_fn_t reply; void *reply_arg; } bridge_reply_hdr_t;
  ```
- New optional field on `bridge_translator_t`: `bool wants_reply;` (default 0).
- When the pump builds a bridge whose translator has `wants_reply`, after
  `ctx_init` it writes `{reply = <send-to-b sink>, reply_arg = <b ctx>}` into the
  first `sizeof(bridge_reply_hdr_t)` bytes of BOTH direction ctxs (the encode ctx
  is the one that needs it; fill both for symmetry). The codec calls
  `hdr->reply(hdr->reply_arg, bytes, len)` to answer the client.
- Codecs that need device state (gvret time/bus/dev-info) get it via a small
  **injected getter** set at `translator_*_init()` time (a function pointer the
  component stores, wired by main to `can_manager_status`/build constants) — keeps
  the codec host-testable (tests inject a stub). NOT globals.

This is ~30 lines in `bridge_manager_pump.c` + 2 fields, fully opt-in. `slcan`
and `raw` set `wants_reply=0` and are unaffected. Document in `bridge_manager.h`.

---

## 7. translator_realdash (RealDash CAN)

Port of `legacy/realdash.c`. New component `components/translator_realdash/`.
RealDash speaks two frame flavors; support both, selectable by a translator name:

- **`realdash44`** — RealDash "44" text-ish binary in: `44 33 22 11 <id:4 LE>
  <data:1..8> <chksum8>` (`real_dash_parse_44`); out uses the 44 frame too.
- **`realdash66`** — CRC32 framed: `66 33 22 11 <id:4 LE> <data:8 padded>
  <crc32:4 LE>` (`real_dash_set_66` / `real_dash_parse_66`). This is the common
  one for RealDash "CAN/serial" custom connections.

Decisions:
- **decode** (CAN-wire → RealDash out): port `real_dash_set_66` (66) — always
  8-byte payload, CRC32. Emit one 20-byte frame per CAN chunk. (44-out is
  rarely needed; 66-out is the RealDash norm — ship 66-out, note 44-out as
  optional.)
- **encode** (RealDash in → CAN-wire): port `real_dash_parse_66` /
  `real_dash_parse_44`, reassembling the fixed-length frame in the ctx (66 = 20
  B, 44 = variable 9..17 B by chksum position). Validate CRC32 / chksum8; bad →
  drop, clean ctx. Emit `can_wire_encode` chunk.
- RealDash is **stateless per frame** and needs **no replies** → no §6 channel.
- Ship the CRC32 table + `chksum8` from the legacy verbatim (they're correct;
  keep as static const in the codec, host-tested against known vectors).

`ctx` (≤64 B): a fixed-size reassembly buffer (max 20) + length + a "flavor"
byte (44/66). Tests: encode/decode round-trip for 66 and 44, CRC/checksum
reject, fragmentation at every boundary, 29-bit id (`id & 0x1FFFF800` → ext, per
legacy).

---

## 8. Component layout & wiring

Each translator is a standard component (COMPONENT_STANDARD §8 kit):
```
components/translator_slcan/
  include/translator_slcan.h        # translator_slcan_init(void)
  translator_slcan.c                # the codec + registration
  CMakeLists.txt                    # PRIV_REQUIRES bridge_manager can_manager
  host_test/{CMakeLists.txt, main/{CMakeLists.txt, test_main.c, idf_component.yml}}
  README.md, TASK_translator_slcan.md
```
Same shape for `translator_gvret`, `translator_realdash`.

Wiring in `main` (historical — this glue now lives in
`components/bridge_endpoints`, and socket jacks register under their
CONFIGURED names automatically):
- `main_endpoints.c`: register the new `"can"` endpoint (+ `"rd0"` socket
  endpoint if a dedicated RealDash socket port is wanted; else reuse an existing
  configurable socket). Add its glue (pump task).
- `main.c` init pass (before `bridge_manager_start`, alongside the other
  `*_register`): call `translator_slcan_init()`, `translator_gvret_init()`,
  `translator_realdash_init()` (each registers its codec; the last two also set
  their injected getter). Add the three components to `main/CMakeLists.txt`
  REQUIRES.
- No new settings component: bridges are configured in the existing
  `bridge_manager` settings array; sockets in `socket_manager`. Example user
  config (via the web UI / API):
  `socket_manager.servers += {name:"slcan0", proto:"tcp", port:3333}` and
  `bridge_manager.bridges += {name:"br_slcan", a:"can", b:"slcan0",
  translator:"slcan", enabled:true}`. (These endpoint names already exist in
  main's glue.)

Registration cap: `BRIDGE_MANAGER_MAX_TRANSLATORS` is 4 — slcan+gvret+
realdash(+realdash44 if split) = 3–4. If both RealDash flavors ship as separate
translators, bump the cap to 5 (one-line change) or fold them into one
`realdash` translator with the flavor auto-detected from the header byte
(`0x44` vs `0x66`) — **preferred: one `realdash` translator, auto-detect on
encode, 66-frame on decode**; keeps the cap at 4.

---

## 9. Testing strategy

| Translator | Host tests | Bench |
|---|---|---|
| slcan | round-trip 11/29/RTR/dlc, fragmentation, multi-frame, malformed, timestamp | **python-can `slcan` + PCAN** (both directions) — do first |
| gvret | replay fixture (whole + re-split), command parse, frame emit, reply bytes, malformed | SavvyCAN ↔ `can↔gvret↔tcp:23` ↔ PCAN (needs the capture) |
| realdash | 44 & 66 round-trip, CRC32/chksum reject, fragmentation, 29-bit | RealDash app custom CAN connection ↔ `can↔realdash↔tcp` ↔ PCAN |

All host suites run under the existing runner (`./test.ps1 host translator_slcan`
etc.; the jsdom-style runner globs `components/*/host_test`). Add a
translator-overhead row to `bridge_manager/BENCHMARKS.md §2` and (gvret) the §5
full-bus 500 kbit/s headline once benched.

---

## 10. Build order / milestones

1. **`can_frame_wire.h`** + its host test (pure, trivial) — the shared foundation.
2. **`can` endpoint** glue in `main_endpoints.c` + pump task. Build-only check.
3. **translator_slcan** + host tests → green. Wire `translator_slcan_init`.
   Build + flash. **python-can + PCAN bench both directions.** ← first real win.
4. **Reply channel** (§6) in bridge_manager + host-test the header-fill.
5. **translator_gvret** + host tests (hand vectors first; fixture when captured).
   Wire + SavvyCAN bench.
6. **translator_realdash** (auto-detect 44/66) + host tests. Wire + RealDash bench.
7. Docs (READMEs, BENCHMARKS rows, memory) + the web UI Diagnostics page gains a
   "CAN bridges" view (bridge list already at `/api/bridges`).

## 11. Open questions for meatpi
- **GVRET dialect/version** SavvyCAN expects — capture a legacy TCP:23 session as
  the fixture before finalizing replies (TASK_gvret).
- **SLCAN control replies:** confirmed not needed for python-can; confirm no
  other target needs `V`/`N`/`F` before shipping absorb-only.
- **RealDash flavor:** ship one auto-detecting `realdash` translator (66 out,
  44/66 in) — confirm 66-out is the desired default.
- **Bus reconfigure from protocol** (GVRET SETUP_CANBUS / SLCAN `S`): v1 absorbs
  (bus owned by can_manager settings). Confirm that's acceptable vs. letting the
  client set the bitrate live.
- **RealDash socket port / endpoint** — dedicated `rd0` or reuse a generic TCP
  server? (default: a user-configured socket_manager server).
```
