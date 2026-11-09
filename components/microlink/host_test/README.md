# microlink host tests

Run from the firmware repo: `.\test.ps1 host microlink`.

The suite compiles two production files against a stub of
`microlink_internal.h` (`main/stubs/`).

`ml_disco_pong.c` with transport, RNG and NaCl test doubles (`test_main.c`,
8 cases): a UDP ping gets exactly one reply to its source and no relay copy,
a relay ping gets exactly one relay reply and no UDP copy, the 110-byte
envelope and 32-byte plaintext encoding, missing UDP addresses, encryption
failure, short/failed UDP writes and relay queue failure. Failures never
fall back to another transport.

`ml_routes.c`, the tunnel's lwIP destination table (`test_routes.c`, 8 cases,
2026-11-09): the tunnel's own network is in the table with no peer at all and
is the netif address masked, the subnet routes follow it, the status list
(`/api/vpn` `routes`) holds the subnet routes only with their owner, an
inactive peer is skipped, more subnet routes than `ML_MAX_ROUTES` set the
dropped flag and keep the own network, a netif without a netmask gives no own
network, and the builder never writes past the buffer it was given.

Expected: `16 Tests 0 Failures 0 Ignored`, `microlink: PASS (16 tests)`.
This tests framing, dispatch and the route list, not NaCl cryptography and
not lwIP's lookup. The real encrypted exchange, blocked-UDP relay traffic,
rekey, direct upgrade, the request to a peer's own address after every
session lapsed and the driver's key ageing are covered by
`.\test.ps1 tspublic` against Betty.
