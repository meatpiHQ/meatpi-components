# can_core host tests

Unity suite for `can_core_filter.c` — the pure CAN frame filter/mask
match, id parse (11/29-bit), byte/byte-string parse, and hex-id formatting.
These have no FreeRTOS/TWAI dependencies, so they run on the IDF `linux`
target.

This is the RX-dispatch logic `can_manager` and the ELM327 CAN path use
(`can_core.c: can_subscription_matches_frame → can_core_filter_match`);
the surrounding TWAI driver + queue glue is hardware and is covered on the
bench, not here.

Run:
```
idf.py --preview set-target linux && idf.py build
./build/can_core_host_test.elf
```
or via the repo runner: `./test.ps1 host` (syncs + runs on rpi001).
