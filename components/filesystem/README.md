# filesystem

## Summary

The firmware's general-purpose file API (core layer, ARCHITECTURE.md §5). Owns the
internal-flash data filesystem — LittleFS on the `storage` partition, mounted at
`/data` — and presents one path-based interface that routes logical paths to the
right backend. `/sd/...` is reserved for the SD card backend and reports
`ESP_ERR_INVALID_STATE` until `external_storage` exists and is wired up (v1 ships
the internal backend only). The `settings` partition is deliberately **not** served
here — `settings_manager` owns it for fault isolation.

Pinned design decisions (closes REVIEW.md §6.1):

- **Path namespace:** callers address a backend by logical prefix (`/data`, `/sd`).
  No transparent fallback between backends.
- **SD absent:** operations on `/sd/*` fail with `ESP_ERR_INVALID_STATE`; callers
  decide their own fallback policy.
- **Writes are always atomic:** `filesystem_write()` = temp file + `fsync` +
  `rename`, and auto-creates parent directories. A power cut leaves the old file
  or the new one, never a torn one.
- **`list` is part of the API** — the settings UI needs it to enumerate candidates
  for the schema `format:"file"` keyword.

## API

| Function | One-liner |
|---|---|
| `filesystem_init()` | Mount the internal data FS (formats on first use). |
| `filesystem_start()` / `filesystem_stop()` | Uniform lifecycle; `stop` unmounts (re-`init` allowed, test flows). |
| `filesystem_write(path, data, len)` | Atomic replace; creates parent dirs. |
| `filesystem_read(path, buf, buf_len, *out_len)` | Whole-file read; `ESP_ERR_INVALID_SIZE` + required size if `buf` too small. |
| `filesystem_size(path, *out)` | File size in bytes. |
| `filesystem_exists(path)` | `bool`; false on any error. |
| `filesystem_delete(path)` | Unlink file / remove empty dir. |
| `filesystem_mkdirs(path)` | Ensure a directory chain exists. |
| `filesystem_list(dir, cb, ctx)` | Enumerate entries (name, is_dir, size); cb can stop early. |
| `filesystem_open(path, mode)` | Validated `FILE*` for streaming (chunked HTTP serving). Streaming writes are **not** atomic. |
| `filesystem_info(prefix, *total, *used)` | Backend capacity. |

Errors: `ESP_ERR_INVALID_ARG` (bad path — wrong prefix, `..`, `//`, trailing `/`,
≥128 chars), `ESP_ERR_INVALID_STATE` (not inited / backend unavailable),
`ESP_ERR_NOT_FOUND`, `ESP_ERR_INVALID_SIZE`, `ESP_ERR_NO_MEM`, `ESP_FAIL`.

## Dependencies

- `joltwallet/littlefs` (managed, private) — the internal FS.
- `vfs` (private) — POSIX layer.
- Init order: `filesystem_init()` runs in `main` after `external_storage_init()`
  (once that exists) and before anything that reads/writes files
  (`download`, `http_server_manager`). See ARCHITECTURE.md §11.

## Settings (`"filesystem"`, version 1)

Minimal descriptor, one knob: `cli` (bool, default true) — register the
`fs` console command with cmdline_manager on the settings boot apply
(reboot-to-apply). Registered via `filesystem_register_settings()`,
wired by main right after settings_manager_init because this component
inits before it (the log_manager_register_settings pattern,
2026-07-05).

## HTTP API (requirement — full endpoint reference: `HTTP_API.md` in this directory; conventions: `components/HTTP_API.md`)

Read-only in v1, via the `api_http` glue: `GET /api/fs/list?path=…` (drives
the settings-UI `format:"file"` picker) and `GET /api/fs/info?path=…`
(capacity). Path validation is this component's own (`fs_path_resolve`).
Write/delete/upload routes are deliberately deferred until an auth story
exists.

## Concurrency & memory

- One recursive mutex serializes all operations. `filesystem_open()` handles are
  caller-owned stdio streams outside that serialization.
- All file data bounces through a 2 KiB **internal-RAM** scratch buffer
  (`// internal: cache-off during FS write`), so caller buffers may live in PSRAM.
- Standard §2 corollary still applies to callers: no `filesystem_write()` from a
  task with a PSRAM stack.

## Memory footprint (estimated — replace with measured before release)

| Where | What | Size |
|---|---|---|
| Internal `.bss` | scratch buffer + mutex | ~2.2 KiB |
| PSRAM `.bss` | state struct | < 32 B |
| Heap (internal) | LittleFS mount (esp_littlefs caches) | ~4–8 KiB while mounted |
| Task stacks | none (runs in caller context) | 0 |

## Tests

- **Host (Unity, `host_test/`):** pure path logic — prefix routing, traversal/`//`
  rejection, length caps, temp-name and parent derivation. `idf.py --preview
  set-target linux && idf.py build` (Linux host required), run the ELF.
- **On-target (`test_apps/`):** mounts the real `storage` partition and exercises
  write/read roundtrip, atomicity (no temp leftover), nested auto-create,
  list/size/delete, bad-path rejection, `/sd` unavailability, and persistence
  across a stop/re-init remount. Builds against the **main firmware's partition
  table** (`wican_pro_partitions_table.csv`) per Coding Standard rev 2.1 §7.

## CLI

`filesystem_register_cli()` (main, CLI builds) registers the `fs` command with cmdline_manager (`filesystem_cli.c`) — per-backend usage for /data and /sd.
