# data_logger

Generic record logger (successor of legacy `obd_logger`): producers
push timestamped records; this component owns storage on the SD card —
batching, file rotation, retention, and the storage-engine choice.
**Two streams** since 2026-07-09 (TASK addendum): numeric params and
raw CAN frames, each with its own format, file series, rotation and
retention — their rates and tooling differ wildly (autopid ≤ ~19
samples/s vs a 500 kbit/s bus peaking ~4,000 fps).

## Model

```c
dl_param_t rpm;
data_logger_register_param("autopid", "rpm", &rpm);   /* once, cheap */
data_logger_write(rpm, 850.0);                        /* never blocks */
```

Records land in PSRAM rings (drop-oldest + counter); one writer task —
the only file toucher, PSRAM stack (SD-only IO) — drains both rings in
batches. A yanked card parks the writer while the rings keep
absorbing; producers never stall. Params and frames have SEPARATE
rings so a CAN flood can never evict param records (frame ring sized
by `ring_len`).

## Producers

- **autopid → params**: the `autopid_log` setting (`off | changed |
  all`). main wires `autopid_set_value_sink(data_logger_autopid_sink)`;
  autopid feeds every accepted sample (with a changed flag), the
  setting picks none / on-change / every-sample. Params appear as
  `autopid.<name>`. For SELECTIVE per-param logging use the rule path
  instead (`on autopid.param match {param} → logger.write`).
- **CAN → frames**: `can_log` subscribes into can_manager's RX fan-out
  (needs can_manager `enabled`; `silent` mode recommended for passive
  logging). `can_filter`/`can_mask`/`can_ext` (hex strings) select
  ids; empty filter = ALL frames. One small drain task moves the
  subscription queue into the frame ring.
- **rules**: the `logger.write {source?, name, value}` event action —
  any event's values, user-selected (see Events below).

## Storage engines (`format` / `can_format` settings)

Each stream binds its own engine instance (any combination, two files
open at once). Params: `sqlite | csv | binary | jsonl`. CAN:
`binary | csv | sqlite | mf4 | blf | candump | asc | jsonl`:

| format | file | streams | why |
|---|---|---|---|
| `sqlite` | `.db` (`params`+`records` / `frames` tables) | both | legacy-compatible `.db` tooling; ~700 rows/s tuned — CAN only for filtered/slow streams |
| `csv` | `.csv` (`ts_ms,source.name,value` / `ts_ms,id,ext,rtr,dlc,data`) | both | spreadsheet/pandas friendly |
| `binary` | `.wdl` packed + inline dictionary | both | THE busy-bus option (~170× sqlite); convert offline: `tools/wdl_dump.py --to csv\|candump\|trc\|text` |
| `mf4` | `.mf4` MDF 4.10 (ASAM) | CAN | the bus-logging standard: asammdf / CANoe / MATLAB / INCA (CANedge-style). CAN_DataFrame channel group; readable to the last commit (in-place DT/CG patching) |
| `blf` | `.blf` Vector BLF | CAN | Vector-shop native, python-can readable; uncompressed LOG_CONTAINERs v1 (zlib = future); header re-patched per commit |
| `candump` | `.log` SocketCAN text | CAN | can-utils replay / SavvyCAN import; absolute epoch stamps |
| `asc` | `.asc` Vector CANalyzer text | CAN | CANalyzer/CANoe text tooling |
| `jsonl` | `.jsonl` one JSON object per line | both | universal (jq / pandas / anything) |

mf4/blf/asc are **single-use** (relative timestamps / patched headers):
every boot/rotation starts a fresh file — no cross-boot append. PEAK
TRC stays offline (`wdl_dump.py --to trc`) unless users ask.

Files live in `/sd/logs`, rotate at `max_file_mb` / `can_max_file_mb`,
and the oldest are deleted beyond `max_files` / `can_max_files` —
retention is per stream (prefix-scoped) and spans engine switches.
Zero-padded epoch names make lexical order = age order.
Browse/download/delete via the existing `/api/fs` routes. The ACTIVE
file of each stream is write-locked (`CONFIG_FATFS_FS_LOCK`,
2026-07-09): downloading or deleting it fails with an error instead of
corrupting the card (unlink-while-open = orphaned FAT chains) — pause
via `/api/logger/gate` (closes + flushes both files) or wait for
rotation, then fetch. Rotated files are always free.

## Settings (`data_logger`, v2, reboot-to-apply)

`enabled` (false), `format` (sqlite|csv|binary|jsonl), `max_file_mb`
(1–32, 4), `max_files` (1–500, 100), `batch_rows` (16–1024, 256),
`flush_ms` (100–60000, 1000), `autopid_log` (off|changed|all, off),
`can_log` (false), `can_format`
(binary|csv|sqlite|mf4|blf|candump|asc|jsonl, binary), `can_filter`
(hex id, "" = all), `can_mask` (hex, "7FF"), `can_ext` (false),
`can_max_file_mb` (1–128, 8), `can_max_files` (1–500, 50), `ring_len`
(512–8192, 2048), `cli` (true). v1→v2 migration is a pass-through;
format-enum ADDITIONS are schema-compatible (no version bump).

## Events (event_manager)

Sources: `logger.rotated {file}` (both streams — the prefix tells them
apart), `logger.error {count}`.
Actions:
- `logger.enable` / `logger.disable` — rule-driven gating ("log only
  while driving"). Paused keeps filling the rings, so an enable rule
  also lands the newest pre-trigger records.
- `logger.write {source?, name, value}` — rule-selected values into
  the params stream, e.g. `match autopid.param -> logger.write
  {"source":"autopid","name":"${param}","value":"${value}"}`.

## Surfaces

- `GET /api/logger` — status JSON: param-stream fields at the top
  level (enabled/running/paused/storage_ok, file, rows, files,
  written/dropped/errors/rotations) + a `can{}` block (enabled, file,
  file_rows, files, queued, frames_written, frames_dropped,
  rotations).
- `POST /api/logger/gate` `{"enabled":bool}` — runtime (non-persisted)
  gate for BOTH streams; the HTTP twin of the `logger.enable`/
  `logger.disable` event actions. Resets on reboot.
- CLI `logger` — both streams' status; `logger test <rows>` queues
  synthetic `test.value` records, `logger frametest <n>` synthetic
  frames (engine throughput without a bus).

## Testing

- Host: `host_test/` (13 tests — file-name/prefix rules, hex filter
  parse, `.wdl` + csv frame-encoder golden vectors).
- Live: `tools/testbench/data_logger_bench.py` (`test.ps1` stage
  `live datalog`) — the full format matrix with CONTENT validation
  (python sqlite3 / csv / wdl_dump byte-exact vs PCAN-sent frames),
  the autopid sink leg, the id filter leg, rotation/retention, the
  runtime gate, and the paced CAN-rate benchmark. CAN legs self-skip
  without a PCAN.

## Why the sqlite pragmas look like that

The vendored port (components/sqlite3, PROVENANCE.md) compiles out
xSync AND WAL; legacy toggled `journal_mode` per store call and got
the DELETE-journal worst case. See `BENCHMARKS.md` for the measured
matrix (legacy 130 → tuned 724 rows/s; littlefs-on-SD rejected,
7–13× slower everywhere; 2026-07-09 end-to-end stream numbers).

## Footprint

PSRAM: ~250 KB static (param ring 12 KB + frame ring 192 KB at the
8192 cap + writer 10 KB + drain 3 KB stacks + registries + CAN queue)
plus ALL sqlite heap (SQLITE_CONFIG_MALLOC → PSRAM). Internal: 4 KB
DMA-capable stdio buffer PER OPEN append-engine file (lazy heap —
allocated at open, freed at close; must be DMA-capable or sdmmc
bounce-buffer allocs fail under pressure) + FreeRTOS objects. The
writer/drain PSRAM stacks are safe because they only touch the SD
card via SDMMC, never internal flash (§2 corollary doesn't apply).
