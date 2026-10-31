# lwip_guard: the resolver guard for IDF's lwIP port (issue 1024)

**Summary.** IDF's lwIP port gives every task that touches sockets or the
resolver a semaphore of its own (`LWIP_NETCONN_SEM_PER_THREAD`): 92 bytes
allocated from internal RAM on the task's first call, kept in the task's
pthread storage. When internal RAM is exhausted that allocation fails, the
port's getter `sys_thread_sem_get()` returns NULL, and lwIP's validity check
`sys_sem_valid()` (a `memcmp` against zeros with no NULL guard, IDF 6.0.2
and 6.1 alike) dereferences it: `LoadProhibited` in ROM `memcmp` at address
0 in the first name lookup of any task. Issue 1024 (2026-10-28, beta.5)
was exactly that, in the HA webhook poster, three boots in a row, and the
crash-loop brake parked the device. This component wraps the getter at link
time so a NULL answer becomes a static zeroed semaphore, which lwIP reads as
"none": `getaddrinfo()` fails with `EAI_FAIL`, a socket operation with
`ENOMEM`, `select()` with `ENOMEM`, and the task lives. Sits with the core
services; `main` calls `lwip_guard_init()` (the log descriptor) before the
network components; the wrap itself is link-time and needs no init.

**How it gets in front of lwIP.** `CMakeLists.txt` adds
`-Wl,--wrap=sys_thread_sem_get` (with the `-u __wrap_sys_thread_sem_get`
that makes the linker take the wrapper out of this static library at its
first scan, the ip4_routes lesson of 2026-10-29): every call from lwIP's
`api_lib.c` (`netconn_gethostbyname_addrtype_n`, `netconn_apimsg`) and
`sockets.c` (`select`, `poll`, `getsockopt`, `setsockopt`) lands in
`__wrap_sys_thread_sem_get`, which calls IDF's getter and only steps in on
NULL. Each of those callers checks `sys_sem_valid()` on the answer before
using it, which is what makes the zeroed semaphore a clean refusal: it is
never waited on or signalled. Re-check the callers at every IDF bump.

**What it does and does not do.** It turns a panic into a failed call and
one E line per 10 s naming the task and the internal free bytes; the device
is still out of internal RAM and the cause must be found. The 2026-10-31
cause was the Tailscale client's DERP relay TLS context leaking on every
refused handshake (about 21 KB PSRAM and 230 B internal per attempt, see
`microlink/PROVENANCE.md` 14 and 15); with BLE on, the internal heap was
gone 13 s after boot. The E line is deliberate: the refusal is a genuine
fault the health net should count (`boot_errors`, `log_errors`).

## API (`include/lwip_guard.h`)

| Function | What |
|---|---|
| `lwip_guard_init()` | the log descriptor; no side effects |
| `lwip_guard_refusals()` | how many times a task was refused its semaphore since boot |

## Dependencies

`lwip` (the semaphore type), `esp_timer` (the clock of the log rate limit),
`log_manager`. No settings, no CLI, no routes.

## Memory footprint

Internal: 92 B `.bss` (the zeroed semaphore, which lwIP must be able to read
as a `StaticSemaphore_t`), 12 B of state, a spinlock. PSRAM: none. No task.
Measured 2026-10-31 by `idf.py size-components`: below 200 B of code.

## Tests

- `host_test/`: Unity tests of the pure core (`lwip_guard_core.c`), the
  refusal counter and the one-line-per-10-s rate limit, run on the linux
  target (`.\test.ps1 host lwip_guard`).
- Bench: the Tailscale plus BLE boot of 2026-10-31 (the reproduction of
  issue 1024, `test-reports/2026-10-31_1145_tailscale_1024_repro.md`)
  rerun on the fixed build: see the TESTING.md row.
