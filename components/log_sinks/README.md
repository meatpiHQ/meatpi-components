# log_sinks (External Log Sinks)

## Summary

Service component owning the log sinks `log_manager` deliberately doesn't
(standard §9.3, Architecture §3 — the log pipeline has no network or
filesystem dependency). It registers four sinks into
`log_manager_add_sink()` at start; each one is gated by its own
**default-false** setting, so the device ships with no log byte leaving
the box.

| Sink | Transport | Consumes |
|---|---|---|
| `tcp` | live log-tail **server** on the device (`nc <dut-ip> 5515`), up to 2 clients | anyone who connects |
| `udp` | **push** to a configured collector `udp_host:udp_port`, one datagram per line batch (≤ 1400 B) | a remote listener (`nc -ul 5514`, rsyslog, etc.) |
| `ws` | text frames on the websocket_manager `ws_log` channel (`/ws/log`) | the web UI / any WS client |
| `file` | rotated plain-text files `/sd/devlog/devlog_<epoch>.log` | post-mortem reads |

Pipeline contract (§9.5): a sink's `write` callback runs inside the log
task and only copies the line into a per-sink PSRAM record ring
(drop-oldest, counted) and notifies a flusher — a stalled TCP client, an
unroutable collector or a slow SD card can never stall the log pipeline.
Two flushers: one network task (PSRAM stack — no flash writes) for
tcp/udp/ws, and one **internal-stack** file writer (§2 corollary, the
log_manager README's wear-safe recipe: flush on ring high-water or the
coarse `file_flush_s` period, one `fflush`+`fsync` per flush, never
per line).

Behavioral notes:

- Lines are the raw formatted log lines (including ANSI color codes —
  strip client-side, the web UI already does).
- `tcp`/`ws` only buffer while a client is connected; `udp`/`file`
  buffer whenever enabled (the file ring absorbs card-absent windows,
  drop-oldest counted).
- A TCP client that stalls past 200 ms is treated as dead and dropped.
- `udp_host` is resolved lazily (5 s retry backoff) and then cached for
  the boot; a collector IP change needs a reboot or DHCP re-lease.
- The sinks enumerate in `GET /api/logs/status` and toggle via
  `PUT /api/logs/sink` like the built-ins — those runtime toggles
  pause/resume a RUNNING sink (ephemeral, §9.4); engine bring-up itself
  is reboot-to-apply via the settings gates. No new HTTP routes.
- Registered sinks count toward log_manager's sink registry
  (`WICAN CAPS log_sinks=6/12` at boot).

## API

| Function | One-liner |
|---|---|
| `log_sinks_init()` | Register the `"log_sinks"` settings + log descriptors. |
| `log_sinks_start()` | Register the four sinks with log_manager; bring up the gate-enabled engines. After `websocket_manager_start()` + `external_storage_start()`. |
| `log_sinks_stop()` | Stop the network flusher (file writer parks on unmount). |
| `log_sinks_stats(id, *out)` | Per-sink counters: `in` / `out` / `dropped` / `buffered` / `detail` (conservation: `in == out + dropped + buffered` once quiesced). |
| `log_sinks_file_flush()` | Event-driven file flush now (§11-sanctioned; CLI `logsinks flush`). |

## CLI (`logsinks`, gated by the `cli` setting)

- `logsinks` — per-sink counter table (the conservation surface the
  bench asserts).
- `logsinks flush` — flush the file sink now.
- `logsinks emit <n> [gap_ms]` — emit `n` numbered `LSBENCH i/n` INFO
  lines (the bench's known generator; default 5 ms gap keeps the
  pipeline below saturation so conservation is exact).

## Dependencies

`log_manager` (sink registration), `settings_manager`,
`websocket_manager` (the `ws_log` channel — added to its defaults in
settings v2 with a v1→v2 migration), `external_storage` (SD
mount-follow), `cmdline_manager`, `lwip` (own TCP/UDP sockets — the
j2534_server ownership model; socket_manager's servers are
bridge-endpoint-oriented and its UDP can't push unprompted).

Boot order: `log_sinks_init()` after `settings_manager_init()`;
`log_sinks_start()` after `websocket_manager_start()` and
`external_storage_start()` (main places it next to `mqtt_can_start`).

## Settings (`"log_sinks"`, version 1)

| Key | Type | Default | Notes |
|---|---|---|---|
| `tcp_enabled` | bool | `false` | live log-tail server gate |
| `tcp_port` | int 1..65535 | `5515` | tail server port |
| `udp_enabled` | bool | `false` | collector push gate |
| `udp_host` | string ≤63 | `""` | collector host/IP — required when `udp_enabled` (`on_validate`) |
| `udp_port` | int 1..65535 | `5514` | collector port |
| `ws_enabled` | bool | `false` | stream onto the `ws_log` channel (`/ws/log`; the channel itself is a websocket_manager setting, default-enabled route) |
| `file_enabled` | bool | `false` | SD file sink gate |
| `file_max_kb` | int 64..4096 | `512` | rotation size cap per file |
| `file_keep` | int 1..16 | `4` | retention: newest N files kept |
| `file_flush_s` | int 5..3600 | `60` | coarse flush period (high-water flushes earlier; wear rule) |
| `cli` | bool | `true` | register the `logsinks` command |

## Memory footprint (measured 2026-07-26, `idf.py size-components`)

| Where | What | Size |
|---|---|---|
| Flash | code + rodata | 5.2 KiB |
| PSRAM `.bss` | rings (8+4+8+16 KiB) + 2 KiB batch buffer + net-task stack (6 KiB) | **45,056 B** |
| Internal static | TCBs, spinlock, flags | **1,041 B** |
| Internal heap | file-writer stack (6 KiB) + 2 KiB DMA drain buffer — allocated ONLY when `file_enabled` | 8.2 KiB when on; 0 otherwise |
| Flash writes | file sink only, batched (≥ `file_flush_s` or ring high-water), SD card only — internal NOR untouched | — |

## Tests

- **Host (`host_test/`, 14 tests):** the pure core — ring push/pop
  roundtrip, FIFO, drop-oldest eviction counting, wrap integrity,
  oversized-record truncation, batcher whole-record budget +
  never-wedges, rotation epoch monotonicity, retention victims/clamps.
  (websocket_manager's host suite gained 3 migration tests for the
  `ws_log` default channel.)
- **Live bench (`tools/testbench/log_sinks_bench_test.py`, runs on
  rpi001; `.\test.ps1 live logsinks`):** gates-closed leg (no listener,
  no bytes), then per-sink legs with the `logsinks emit` known
  generator — TCP tail conservation (exactly N marker lines), UDP
  collector conservation, WS frame stream, file sink content +
  rotation + retention on the card, counter identity
  `in == out + dropped` per sink, restore + faults clean, zero-error
  budget.
