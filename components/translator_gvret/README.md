# translator_gvret

The **SavvyCAN GVRET** binary CAN codec, as a `bridge_manager` translator
(`"gvret"`). Lets SavvyCAN connect to WiCAN's CAN bus over TCP/USB/WS.

- `decode` (can→SavvyCAN): CAN-wire chunk → GVRET frame record
  `F1 00 <ts:4> <id:4, bit31=ext> <dlc> <data> <xor-chk>`.
- `encode` (SavvyCAN→can): the F1-prefixed command stream (reassembled in the
  ctx) → CAN TX frames (BUILD_CAN_FRAME) **plus handshake replies**
  (TIME_SYNC / GET_CANBUS_PARAMS / GET_DEV_INFO / KEEPALIVE / GET_NUMBUSES /
  …) sent back to the client via the bridge **reply channel** (`wants_reply`).
  SETUP_CANBUS is parsed + absorbed (the bus is owned by `can_manager`).

The **reply channel** (`bridge_reply_hdr_t`, `bridge_manager.h`) is the clean,
opt-in mechanism for protocols that must answer the client on the near side;
the pump fills it after `ctx_init`. Design: `DESIGN_translators.md §5/§6`.
Registered by `translator_gvret_init()`.

## Use
```
bridge_manager.bridges += {name:"br_gvret", a:"can", b:"gvret0", translator:"gvret"}
```
`a` = CAN endpoint. SavvyCAN → "Connect" → GVRET over TCP to the socket.

## Tests
- Host: `./test.ps1 host translator_gvret` — decode records (checksum, ext
  bit31), encode BUILD_CAN_FRAME (+ fragmentation), handshake replies via the
  reply channel.
- Bench (pending SavvyCAN): capture a legacy TCP:23 session as
  `fixtures/savvycan_session.bin`, replay in the host test, then SavvyCAN ↔
  `can↔gvret↔tcp` ↔ PCAN live. Confirm the GVRET dialect first (TASK §Mission).
