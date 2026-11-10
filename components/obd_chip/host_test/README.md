# obd_chip: host unit tests

Two layers in one binary, run via `.\test.ps1 host obd_chip`:

- the pure modules (`obd_chip_parse.c`, `obd_chip_class.c`, `obd_chip_fw_version.c`, `obd_chip_stslcs.c`: no UART/GPIO);
- since 2026-11-10 the production request engine, claim model and fan-out (`obd_chip_cmd.c`, `obd_chip_claim.c`, `obd_chip_fanout.c`) against a fake chip that stands in for the UART (`test_request.c`). The fake collects bytes in a line buffer up to a CR as the real chip does, answers `OK` to AT words, data to `0100` and `?` to anything else; bytes left in its buffer without a CR merge with the next word, which is how a real chip comes to refuse the driver's `ATM0`. The tests assert the words on the wire. The bring-up gate, the obd_gate and the clock are stubs in that file.

## What is covered (40 tests)

| Case | What it proves |
|---|---|
| simple response | echo + payload + `>` prompt → clean extract |
| echo-off response | works after ATE0 (no echo present) |
| prompt split across chunks | every split point 1..20 reassembles identically |
| bytes after prompt not consumed | stream position hands the next transaction its bytes |
| unsolicited noise interleave | broadcast contract: noise inside the window is kept verbatim, prompt still terminates |
| **long real-car response, all splits** | meatpi's genuine 36-line `22202A` log replayed whole and at chunk sizes 1,2,3,7,16,64,127,128,4096: first/last lines intact, all 36 present |
| overflow flagged not fatal | >4 KB responses truncate with `overflow`, still terminate |
| chip error classification | `?`, UNABLE TO CONNECT, CAN ERROR vs valid data |
| monitor command classification | ATMA/ATMR/ATMT/STM(A) incl. case/space/args; ATI/0100/ATMAX are not |
| fw iterator + end marker | CR/LF stripping, empty-line skip, `FFF1` detection |
| fw version policy | (issue #1023) a chip at V2.3.22 or V2.3.24 is current (the v4.52p list), V2.3.18/V2.3.10, `?`, empty and NULL are not; the log string names both. 2026-11-01 (Ali: no forced rewrite): the version numbers parsed out of a VTVERS answer, the packaged constant or a file's path (`VTVERS`, a lower-case `v`, `V2.3`, `?`, empty and NULL are none), the version alone as a token (`V2.3.24`, `""` without room), the order numeric field by field (V2.3.9 below V2.3.24), and `is_outdated` = the chip's version below the image's: V2.3.22, V2.3.18 and V2.3.9 under V2.3.24 are, a chip at V2.3.24 under `/data/V2.3.99.txt` is; the image's own version, a newer chip (V2.3.26), a chip at V2.3.24 under a V2.3.22 file, silence, `?`, an image without a version and NULL are not |
| eeprom guard, reset junk, PP parse, STSLCS (5 cases), the `OK` acknowledgement, VTPPSWS | see `test_parse.c` / `test_stslcs.c` |
| protocol switch classification | (issue #1023, 2026-11-01) the words the request engine sends `ATM0` ahead of: `ATTP6` / `attp7` / `AT TP 8` / `ATTPA6` / `ATSP00` / `ATTPC`, `STP33` / `stp 34`, `VTP234`, a chain's first element; not `ATM0`, the resets, `ATDPN`, `ATSH`, `ATST`, the STN/vendor reads (`STPR`, `STPBR`, `STPC`, `VTPRON`), data, junk; the guard's `ATSP7` -> `ATTP7` and the classification agree |
| reset word classification | (issue #1023, 2026-11-01, Ali's ATZ-as-ATWS) a chunk that is exactly an app's `ATZ` / `ATWS` / `ATD` (any case and spacing, line ends and blanks around it) is one; a chunk with a second command behind it, `ATDPN` / `ATDP` / `ATD0` / `ATD1` / `ATWS1`, other words, junk and NULL are not; only the first `len` bytes count |
| request: the switch behind an acknowledged `ATM0` | the wire reads `ATM0`, `ATTP7`; `memory_off` +1, nothing refused, the generation still |
| **request: the switch not sent when `ATM0` answers `?`** | (the reproduction, 2026-11-10) `XX` in the chip's line buffer: the wire reads `ATM0` alone, `ESP_ERR_INVALID_RESPONSE`, an empty answer, `refused` +1, a switch owed, the generation moved. On the driver before the change the switch went out and the call returned `ESP_OK` |
| **request: the switch not sent when `ATM0` times out** | (the reproduction) a chip silent to `ATM0`: `ESP_ERR_TIMEOUT`, the wire reads `ATM0` alone. Before the change: `ESP_OK`, the switch on the wire |
| request: `OK` and nothing else | `NOT OK`, `STOPPED`, `BUS BUSY`, an `OK` with a banner behind it and an empty answer each refuse the switch; an `OK` with CR and LF around it is taken, the guard's `ATSP6` going out as `ATTP6` |
| request: bus requests held while a switch is owed | after a refusal `0100` and `22F190` return `ESP_ERR_NOT_FINISHED` with nothing on the wire and the generation moved; `ATSH7DF` and `ATCRA` pass; the next acknowledged switch ends it and `0100` is answered |
| request: an owed switch ends with the echo on | the chip echoing and the guard rewriting (`ATSP6` sent as `ATTP6`): the answer carries the echo ahead of `OK` and is still taken |
| request: inside the caller's transaction | the pair rides the caller's hold, a refusal leaves the hold with the caller, `txn_end` frees the chip |
| request: no hold, no switch | a monitor session on the chip: `ESP_ERR_INVALID_STATE`, neither `ATM0` nor the switch on the wire |
| request: a bring-up clears what is owed | `obd_cmd_memory_reset()`: `0100` goes out again |
| **save: a lost answer is not written twice** | (the reproduction, 2026-11-10) the fake chip takes `ATSP6` and its `OK` is lost: `ESP_ERR_TIMEOUT`, the stored protocol not known; the second call of the boot `ESP_ERR_INVALID_STATE` with one `ATSP6` on the wire and one write (before the change: two writes); the next start reads `6` and the save then sends nothing (`protocol_kept`) |
| save: a refused word waits for the next start | `XX` in the line buffer, the chip answers `?`: `ESP_FAIL`, nothing written, no second attempt in that boot; the next start reads the old value and sends its one `ATSP` |
| save: not sent when the chip holds it | the bring-up read `6`, `A6`, `a6`, ` A6 `, `A`, `AA`, `9`: the save of that protocol returns `ESP_OK` with nothing on the wire |
| save: one write when the chip holds another | read `A7`, `7`, `A0`, `34 [ST]`, `?`, an empty answer or none: one `ATSP6`, acknowledged, the store known as `6`; asked again: held, nothing sent; another protocol in the same boot: `ESP_ERR_INVALID_STATE` |
| save: blocked by a monitor spends nothing | no hold, no word, and the boot's one attempt still there |
| save: what is not a protocol | `0`, `D`, `x`, a blank, NUL: `ESP_ERR_INVALID_ARG`, nothing sent |
| `ATDPN` parse | `6`, `A6` (automatic), `A` (J1939, not automatic), `AC`, `0`, blanks and line ends around; not `34 [ST]`, `11A [VT]`, `34`, `D`, `AD`, `?`, empty, `STOPPED`, NULL |
| bus request classification | hex data, a bare CR, `ATFI` / `at si`, `ATRTR`, `STPX`, `VTFullyRequestCk`, a chain starting with data are bus requests; `ATSH`, `ATCRA`, `ATST96`, `ATS1`, `ATFCSM1`, `ATRV`, the protocol words, the resets, `STP33`, `STPBR`, `STFAP`, the `VT` reads and a chain starting with `ATSH` are not |

## Expected result

```
40 Tests 0 Failures 0 Ignored
OK
```

Lines of the driver's own log come with it (the W of a refused `ATM0` and the I of the recovery, the W and I lines of the saves): they are the tested behaviour. The lost-answer case waits out the save's 2 s budget once.
