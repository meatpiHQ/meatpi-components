# sqlite3 — vendored component

Vendored 2026-07-07 from `components/legacy/nopnop2002__sqlite3`
(nopnop2002's esp-idf-sqlite3 port of siara-cc/esp32_arduino_sqlite3_lib,
sqlite 3.x amalgamation + ESP32 VFS in esp32.c). Sole consumer:
`data_logger`.

Port config that MATTERS (config_ext.h — see data_logger/BENCHMARKS.md):
- SQLITE_NO_SYNC 1 — xSync compiled out; `PRAGMA synchronous` is a no-op.
- SQLITE_OMIT_WAL 1 — `journal_mode=WAL` fails silently; use MEMORY.
- SQLITE_DEFAULT_PAGE_SIZE 512, SQLITE_TEMP_STORE 1, THREADSAFE 0
  (callers must serialize on one connection — data_logger's writer task
  is the only toucher).
Benchmarked as-is on the bench card 2026-07-07; left untouched.

Local changes vs legacy copy: dropped screenshots/component.mk; CMake
PRIV_REQUIRES trimmed of unused console/spiffs deps.
