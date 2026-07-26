# berry — vendored Berry VM

Vendored 2026-07-07 from **github.com/berry-lang/berry** commit
`bd9c93b65dfadddc27e3203fc04e60e986a0fa5f` (MIT). Berry is a tiny
embedded scripting language (the Tasmota scripting engine). Sole
consumer: `script_engine` (WiCAN's UDS/automation scripting). Design +
bindings: `components/uds_manager/TASK_uds_manager.md` §5 and
`components/event_manager/SCRIPTING.md`.

## Local changes (this repo)

1. **coc codegen pre-generated + vendored.** Berry's build normally
   runs `tools/coc/coc` (a python "constant object compiler") to
   generate `generate/*.h`. Those 23 headers were generated ONCE
   (`python tools/coc/coc -o generate src -c default/berry_conf.h`) and
   committed, so the IDF build needs no build-time python codegen. If
   `berry_conf.h` or the core sources change, regenerate.
2. **berry_conf.h (default/):** `BE_EXPLICIT_MALLOC/REALLOC/FREE` →
   `berry_port_malloc/realloc/free` (PSRAM via heap_caps — the VM heap
   and working set live in PSRAM, internal RAM is scarce);
   `BE_USE_FILE_SYSTEM = 0` and `BE_USE_OS_MODULE = 0` (scripts use our
   bindings, not VFS/POSIX — avoids the file/dir dependency).
3. **be_port.c (default/):** `be_writebuffer` patched to route script
   output through `berry_port_write` (capture sink for /api/scripts +
   the log) instead of stdout. The file's `be_fopen/fread/fwrite/
   fclose` are KEPT — Berry's core (be_bytecode/be_exec/be_debug)
   references them regardless of the fs module.
4. **berry_esp_port.c (NEW):** the ESP glue — PSRAM allocator funcs +
   `berry_port_write` (capture + line-buffered log) + a settable
   capture sink (`berry_port_set_capture`).
5. **CMakeLists.txt (NEW):** IDF component; compiles `src/*.c` +
   `default/{be_port,be_modtab}.c` + `berry_esp_port.c`; includes
   `src`, `default`, `generate`. Third-party -Werror relaxations.

The REPL main (`default/berry.c`) and the host tools/tests/examples are
NOT vendored (only the embeddable VM + our port).
