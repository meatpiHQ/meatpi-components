# autopid — host unit tests

Pure modules only (`autopid_sched.c`, `autopid_resp.c`, the parse half
of `autopid_config.c`, compiled with `AUTOPID_HOST_TEST` — no chip, no
filesystem). Run via `.\test.ps1 host autopid`.

## What is covered (29 tests)

| Case | What it proves |
|---|---|
| **one request per PID per period** | THE legacy fix #1 regression: a 5-parameter PID over a simulated 10 s produces ~10 requests at 1 Hz, not 50 — parameters aren't schedulable units |
| group inheritance + override | PID period 0 inherits the group; per-PID period wins; a runtime group override retargets the whole group (§5b action) |
| group toggle gates entries | disabled group hides its entries from `ap_sched_next`; the ephemeral flip re-exposes them |
| type gates | `std_enabled=false` hides STD PIDs |
| high-fidelity round-robin | two period-0 PIDs strictly alternate (last_run tiebreak) |
| fail backoff | 3 consecutive fails → period ×4; one success restores |
| stagger | initial due times spread by 10 ms so big tables don't burst |
| headers-off single frame | `41 0C 1A F8` → 4 payload bytes (service echo INCLUDED, B0=0x41) |
| SEARCHING then data | noise lines skipped |
| headers-off ISO-TP | `014` + `0:`/`1:`/`2:` rows → concatenated, trimmed to announced length |
| headers-on single | `7E8 06 41 00 …` → PCI length honored (6 bytes) |
| lowest responder wins | 7E9 + 7E8 both answer → only 7E8 kept (legacy rule) |
| headers-on ISO-TP multiframe | FF `10 14` + CFs `21/22` reassembled to 0x14 bytes (VIN) |
| error lines | NO DATA / CAN ERROR / `?` → ESP_FAIL; only-noise/empty → ESP_ERR_NOT_FOUND |
| cross-talk guard | `ap_payload_matches_cmd`: payload must echo (service\|0x40)+identifier (010C vs 0105, UDS 22xx DIDs, mode 03, response-count hint digit); AT/ST/VT unchecked |
| ATMA filter frames | legacy header shapes (contiguous `123`/`18DAF110`, byte-split ids), id mismatch skipped, the REAL bench `<DATA ERROR` suffix (markers skipped, bytes kept), noise/header-only rejected |
| **filter stream: busy bus** | crowded monitor (many other ids + noise + pre-ATCRA buffered frames), target = ONE frame among them, fed at EVERY chunk size 1..200 — chunk-boundary reassembly can't miss it |
| filter stream: duplicates | same id repeating in one window → FIRST frame wins (collector stops); a fresh window picks up the newest traffic |
| filter stream: mixed DLC | other ids with DLC 1..8 around a DLC-2 target — line length never confuses the match; captured length = the frame's REAL length |
| filter: short-DLC bounds | expression beyond the captured frame (`B6` on 2 bytes, `[B0:B3]`, bit refs) = ESP_ERR_INVALID_SIZE — parameter skipped, never cached as garbage; in-range params on the same frame still evaluate |
| filter stream: truncation | an overlong corrupt line (>160 chars) dropped WHOLE; the next line still parses |
| filter monitor bounds | `monitor_ms` outside 50..60000 rejected at config parse |
| std expression mapping | legacy bit_start/scale/offset → v6 expressions (`[B2:B3]*0.25`, `B2-40`, `B3*0.78125-100`); placeholders refused |
| **std whole-table sweep** | every non-placeholder row of the vendored SAE table generates an expression the REAL parser accepts (no exponent literals, byte refs 2..8) |
| std scan-bitmap parse | `41 00 …` rows headers on/off, SEARCHING noise, multi-ECU OR-merge, wrong-PID echo/NO DATA/truncation rejected |
| config happy path | groups/pids/filters/params counted, auto `default` group, type map, group refs, is_extended |
| bad expression rejected | `[B3:B0]` fails validation; tables wiped on failure |
| unknown group + duplicate params rejected | referential integrity + cache keying |
| empty + garbage | `{}` = valid empty tables; non-object JSON = INVALID_ARG |

## Expected result

```
29 Tests 0 Failures 0 Ignored
OK
```
