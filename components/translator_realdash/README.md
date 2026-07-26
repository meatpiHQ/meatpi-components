# translator_realdash

The **RealDash** CAN framing codec, as a `bridge_manager` translator
(`"realdash"`). Feeds the RealDash app a CAN stream and accepts its frames.

- `decode` (can→app): CAN-wire chunk → RealDash **66** frame
  (`66 33 22 11 <id:4 LE> <data:8> <crc32:4>`, 20 bytes).
- `encode` (app→can): RealDash **44** (`44..`, chksum8, 17 B) **or** **66**
  (`66..`, CRC32, 20 B) frames — flavor auto-detected from the header byte,
  reassembled in the ctx, checksum-validated. 29-bit id when `id & 0x1FFFF800`.
- Stateless per frame → no reply channel. CRC32/chksum8 ported verbatim from
  the legacy WiCAN impl.

Design: `bridge_manager/DESIGN_translators.md §7`. Registered by
`translator_realdash_init()`.

## Use
```
bridge_manager.bridges += {name:"br_rd", a:"can", b:"<tcp/ws endpoint>", translator:"realdash"}
```
`a` = CAN endpoint. Point RealDash at the socket as a "CAN/serial" custom
connection.

## Tests
- Host: `./test.ps1 host translator_realdash` — 66/44 round-trip, CRC32 &
  chksum8 reject, byte-fragmentation, resync after garbage, 29-bit id.
- Bench: RealDash app ↔ `can↔realdash↔tcp` ↔ PCAN (manual; app required).
