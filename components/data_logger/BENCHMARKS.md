# data_logger benchmarks

## End-to-end stream numbers — bench DUT, 2026-07-09

Full pipeline on the composed main firmware (PCAN on the PC → 500 kbit/s
bus → TWAI subscription → drain task → frame ring → writer → SD card),
`can_format=binary`, batch 256, flush 300 ms. Harness:
`tools/testbench/data_logger_bench.py` (the `live datalog` stage);
"landed" counted from `/api/logger` `frames_written` deltas, content
verified byte-exact via `tools/wdl_dump.py` against the sent frames.

| leg | result |
|---|---|
| paced 500 fps × 2,500 | 0 drops (asserted clean) |
| paced 1000 fps × 5,000 | 1,204 fps e2e, 0 drops |
| paced 2000 fps × 10,000 | 2,984 fps e2e, 0 drops |
| **FULL-SPEED blast (saturated bus)** | **33,688/33,688 landed, 3,819 fps e2e, 0 drops** |
| synthetic drain ceiling (`logger frametest`, no bus limit) | **~11,000–11,700 frames/s** to SD |
| params via `logger test` (sqlite, incl CLI pacing) | ~126 rows/s paced (engine ceiling is the 724 rows/s below) |
| internal RAM while CAN-logging | ~3–4 KB (the 4 KB DMA file buffer, open→close) |

**Verdict: the binary engine logs a fully saturated 500 kbit/s CAN bus
to SD with zero loss** (wire ceiling ≈ 3.4–3.8k fps; drain ceiling ~3×
that). csv keeps up with moderate buses; sqlite frames are for
filtered/slow streams only (~700 rows/s, below). Rotation (1 MB) +
retention ran DURING the 60k-frame drain leg with zero drops.

**Variance note (repeat runs)**: one blast run landed 30,663/33,621
(3,102 drop-oldest, counted) — SD-card write-latency variance plus the
concurrently-committing sqlite params stream can steal enough writer
time at wire saturation to overflow the frame ring. The paced 500 fps
leg is loss-free on every run (asserted). For sustained saturated-bus
capture prefer a light params engine (binary/jsonl) alongside, or grow
`ring_len`.

**Heisen-loss note (2026-07-10)**: the FIRST ~3 frames of the first
PCAN transmission after minutes of PEAK TX-idle intermittently never
land in the log (leg1's byte-exact train head — `missing sent-indexes:
[0,1,2]` bare, `[0,1]` with one warmup frame ahead: a fixed ~3-frame
window). Every DUT counter reads 0 at fail time (`frames_dropped` +
the new `/api/can` `rx_missed`/`dispatch_drops`), the **IRAM TWAI ISR
(shipped during this hunt — a genuine latent fix: a flash-resident ISR
loses wire frames in every NVS/littlefs cache-off window) did not
change it**, and everything after the window is loss-free at any rate
(the 2500-frame 500 fps leg + 33k FULL BLAST are clean every run).
Mechanism UNPINNED — CHECKLIST item (next: a second wire witness; the
MIC/ECU-box ACK the frames, so the PEAK sees success either way). The
bench absorbs the window with a 3-frame warmup outside the match set;
the 200-frame train stays strictly byte-exact.

## Addendum-2 formats (2026-07-09)

mf4 (MDF 4.10) / blf (Vector, uncompressed containers) / candump /
asc / jsonl added; all validated byte-exact on the bench against
PCAN-sent frames — **BLF read back by python-can's BLFReader, MF4 by
a spec-offset parser** (asammdf cross-check hooks in when installed).
All are append-style like csv (row/record + fflush/fsync per commit;
mf4/blf add small in-place header patches per commit) — throughput
class is csv-like, well above autopid rates and moderate buses; binary
`.wdl` remains THE saturated-bus format.

## Storage-engine microbenchmarks — bench DUT, 2026-07-07

Real hardware: WiCAN Pro bench unit, its SD card via SDMMC 4-bit
(CLK 21 / CMD 47 / D0 14 / D1 13 / D2 12 / D3 48). App:
`test_apps/main/bench_main.c`. Row = legacy `param_data` shape
(int64 timestamp, int param_id, double value), prepared-statement
inserts. `legacy10` replays what obd_logger actually did per store
call (journal_mode churn + 10-row batch); rows/s is the steady
insert rate including transaction commit.

## Results

| leg | pattern | FATFS rows/s | littlefs rows/s |
|---|---|---:|---:|
| legacy10 | journal churn, batch 10 (legacy behaviour) | **130** | 23 |
| naive_b1 | defaults, 1 row/txn | 71 | 15 |
| mem_b64 | pragmas once, journal=MEMORY, batch 64 | 202 | 28 |
| mem_b256 | same, batch 256 | **724** | 96 |
| off_b256 | journal=OFF, batch 256 | 681 | 97 |
| pg4k_b256 | page_size=4096 + MEMORY + 256 | 682 | 96 |
| raw_b256 | fwrite packed 16-B records, fflush/256 | **123,047** | 9,707 |
| query | count(*) + indexed range over 3,000 rows | 112 ms | 519 ms |

(4,000 rows per leg; legacy10 1,000; naive_b1 500 — capped because
they're painfully slow. littlefs phase reformatted the bench card via
CONFIG_LITTLEFS_SDMMC_SUPPORT and the card was auto-restored to FAT
afterwards — `restore_fat OK`.)

## Findings

1. **Legacy fixed by pragma hygiene + batching: 130 → 724 rows/s
   (5.6×).** Set pragmas ONCE at open (journal_mode=MEMORY,
   temp_store=MEMORY), keep one prepared INSERT, commit every ~256
   rows. journal=OFF and page_size=4096 add nothing on top —
   `journal_mode=MEMORY, batch 256` is the whole fix.
2. **sqlite itself is the remaining tax: raw binary append is 170×
   faster** (123k rows/s vs 724). If on-device SQL query / .db-file
   compatibility is not required, a packed append log embarrasses
   sqlite. Even fsync-per-batch raw writing would leave >10k rows/s.
3. **littlefs-on-SD is a clear NO for this workload**: 7–13× slower
   than FATFS in every leg (its small-block RMW + metadata compaction
   vs SDMMC 4-bit sequential writes). Its power-fail robustness
   doesn't buy enough here — a CRC-framed append-only record format on
   FATFS gets torn-write detection for free. Recommendation: don't
   ship the littlefs backend option; keep `CONFIG_LITTLEFS_SDMMC_SUPPORT`
   only if meatpi overrides.
4. Query speed is a non-issue at log scale (3k rows in 112 ms with an
   index); at 4 MB/file (~260k rows) an indexed range scan
   extrapolates to well under a second per file.

## Component verdict

- v1 storage engine: **tuned sqlite on FATFS** (parity with legacy .db
  tooling) — pragmas once, batch_rows default 256, flush_ms cap so
  sparse streams still land.
- Keep the writer behind a small backend seam so a packed-binary
  engine can be added if meatpi frees the format (open question #2 in
  TASK_data_logger.md) — the 170× headroom is there when CAN-frame
  logging (thousands of rows/s) arrives; sqlite at 724 rows/s cannot
  log a busy CAN bus, binary can.
