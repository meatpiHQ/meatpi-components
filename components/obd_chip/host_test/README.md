# obd_chip — host unit tests

Pure framing/classification only (`obd_chip_parse.c` — no UART/GPIO). Run via
`.\test.ps1 host`.

## What is covered (10 tests)

| Case | What it proves |
|---|---|
| simple response | echo + payload + `>` prompt → clean extract |
| echo-off response | works after ATE0 (no echo present) |
| prompt split across chunks | every split point 1..20 reassembles identically |
| bytes after prompt not consumed | stream position hands the next transaction its bytes |
| unsolicited noise interleave | broadcast contract: noise inside the window is kept verbatim, prompt still terminates |
| **long real-car response, all splits** | meatpi's genuine 36-line `22202A` log replayed whole and at chunk sizes 1,2,3,7,16,64,127,128,4096 — first/last lines intact, all 36 present |
| overflow flagged not fatal | >4 KB responses truncate with `overflow`, still terminate |
| chip error classification | `?`, UNABLE TO CONNECT, CAN ERROR vs valid data |
| monitor command classification | ATMA/ATMR/ATMT/STM(A) incl. case/space/args; ATI/0100/ATMAX are not |
| fw iterator + end marker | CR/LF stripping, empty-line skip, `FFF1` detection |

## Expected result

```
10 Tests 0 Failures 0 Ignored
OK
```
