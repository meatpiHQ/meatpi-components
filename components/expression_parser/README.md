# expression_parser — the Automate expression evaluator (utility)

Pure C evaluator for the WiCAN parameter-expression grammar
(meatpihq.github.io/wican-fw/config/automate/usage) — the formulas users
attach to PIDs/CAN filters (`[B0:B1]/4`, `B0-40`, `(B4:0)*V`). Adopted
from the field-proven legacy evaluator (autopid_legacy, shunting-yard,
static stacks) as its own component per meatpi 2026-07-06, with **the
payload length as a parameter** — the legacy API read past the response
buffer on out-of-range byte references. First consumer: `autopid`
(TASK_autopid.md Phase 0); also the save-time validator behind the future
UI's inline expression check.

## API

| Call | Behavior |
|---|---|
| `expression_parser_eval(expr, data, data_len, v, &result)` | Evaluate against a payload. `ESP_ERR_INVALID_ARG` = malformed expression; `ESP_ERR_INVALID_SIZE` = a byte reference beyond `data_len`; `ESP_FAIL` = division by zero. |
| `expression_parser_check(expr, &max_byte, err, err_len)` | Data-less dry run (same core, byte refs read as 0, `/0` ignored): validates grammar, reports the highest byte index referenced (`SIZE_MAX` = none) + a human-readable reason for the UI. |

## Grammar (legacy-compatible)

`B<n>` unsigned byte · `B<n>:<bit>` single bit 0–7 · `S<n>` signed byte ·
`[B<x>:B<y>]` unsigned big-endian multi-byte (≤ 8 bytes) ·
`[S<x>:S<y>]` signed, **container by span exactly as legacy**: 1 B→int8,
2 B→int16, 3–4 B→int32 (a 3-byte span therefore has no reachable sign
bit — kept, the legacy vectors pin it), 5–8 B→int64 ·
`V` battery voltage · decimal literals · `+ - * / << >> & | ^ ( )` with
the legacy precedence (`* /` > `+ -` > `<< >>` > `&` > `^ |`, left-assoc);
bitwise/shift operands truncate to 32-bit int as legacy did.

Deliberate differences from legacy (all error where legacy read garbage):

- Byte/bit/range references beyond `data_len` → `ESP_ERR_INVALID_SIZE`.
- Inverted ranges (`[B3:B0]`) and bit indexes > 7 → `ESP_ERR_INVALID_ARG`
  (legacy silently produced 0 / nonsense).
- Unary minus is DEFINED at expression start and after `(` (legacy
  accepted exactly those by accident via an empty-stack pop); an operator
  followed by `-` (`3*-2`) stays an error, as legacy behaved — write
  `3*(0-2)`.

## Dependencies

None (esp_err.h only). No logging — errors are return values; `check()`
carries the message. Compiles unchanged on the linux host target.

## Memory footprint (measured on host, static)

Zero globals. Per-call stack: ~1.2 KB (two 64-slot fixed stacks in the
context struct) — fine on any caller stack, no heap ever.

## Tests

Host suite (`host_test/`, 16 tests): the complete legacy vector table
(grammar compatibility incl. the signed-container semantics), docs-page
formulas, unary-minus set, the length-parameter adversarial set
(out-of-range refs on short/empty payloads), malformed-expression set,
`check()` max-byte/dry-run semantics, and precedence pinning.
Run: `.\test.ps1 host expression_parser`.
