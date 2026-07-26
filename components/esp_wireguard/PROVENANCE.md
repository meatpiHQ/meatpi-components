# esp_wireguard — vendored component

Vendored 2026-07-07 from `components/legacy/esp_wireguard` (originally
the trombik/esp_wireguard port with reference-C crypto: blake2s,
chacha20poly1305, x25519, curve25519 smult). Upstream API untouched —
`esp_wireguard.h` (init/connect/set_default/peer_is_up/disconnect).
`examples/` dropped. Sole consumer: `vpn_manager`.

2026-07-17 — the WireGuard CORE this component links
(`components/wireguard_lwip`, shared with microlink/Tailscale) had lost
the legacy tree's `underlying_netif` mechanism: outer encrypted UDP was
sent with `udp_sendto()` (routed), so with `default_route` enabled the
outer packet routed back into the WG netif — infinite encapsulation
recursion → tcpip stack overflow ~25 s after connect (first keepalive).
Re-ported into `wireguard_lwip/src/wireguardif.c`: pin the uplink netif
at init, validate it per send (fall back to any ready non-WG netif for
WiFi→LTE failover), and send outer packets with `udp_sendto_if()`.
Reproduced + verified on bench against a public endpoint over LTE.
