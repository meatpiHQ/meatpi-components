# mqtt_manager — the MQTT client owner (feature)

Owns THE one MQTT client (esp-mqtt): broker connection, reconnect, TLS,
and the device's status contract. Components don't create clients — they
publish through this one and register topic-filter HANDLERS into it
(ownership inversion). `autopid`, CAN↔MQTT bridging, and event alerts
(battery/IMU → MQTT) all build on this surface later.

**IDF v6 note**: esp-mqtt was un-bundled (like cJSON) — it comes from the
registry as the `espressif/mqtt` managed component (`idf_component.yml`).

## Preserved legacy on-wire contract (live-verified 2026-07-04)

- Status topic `<prefix>/status`: retained `{"status": "online"}` on
  connect, retained LWT `{"status": "offline"}` — byte-identical payloads
  (existing dashboards keep working). Full cycle verified against
  mosquitto on rpi001: online → device reboot → broker publishes offline
  (LWT) → online.
- Default prefix `wican/<device_id>`, default client id
  `wican_<device_id>`; keepalive 30 s; auto-reconnect 5 s.

## API

| Call | Behavior |
|---|---|
| `mqtt_manager_init()` | Settings + log descriptors. No network. |
| `mqtt_manager_start()` | Builds the client; a starter task waits for `DEV_STATUS_NETWORK_CONNECTED` then connects (esp-mqtt owns reconnects). No-op when disabled. |
| `mqtt_manager_stop()` | Disconnect + clear the bit. |
| `mqtt_manager_connected()` | Broker session up (mirrors `DEV_STATUS_BIT_MQTT_CONNECTED`). |
| `mqtt_manager_topic_prefix()` | The resolved prefix — consumers build topics on it. |
| `mqtt_manager_publish(topic, data, len, qos, retain)` | DIRECT path: thread-safe, but the socket write runs in YOUR context. Occasional messages only. |
| `mqtt_manager_publish_async(topic, data, len, qos, retain)` | HOT path: one copy into a bounded 32 KB PSRAM ring, returns immediately — **never blocks, never touches the network in your context**; the publisher task drains it. Full/offline → drop-and-count. |
| `mqtt_manager_stats(*out)` | published / dropped_full / dropped_offline. |
| `mqtt_manager_register_handler(filter, cb, arg)` | Topic-FILTER subscription (`+`/`#`, MQTT-spec matching incl. the `$`-topic rule — pure matcher, host-tested). Subscribed on every (re)connect; immediate when already connected. ≤8, lifetime registrations. |

Handler context: the esp-mqtt event task — keep callbacks SHORT, copy out
(queue to your own task). Payloads over the 4 KB RX buffer arrive
fragmented and are dropped+counted in v1.

## High-rate producers (the CAN→MQTT design)

The division of labor, learned from the legacy firmware:

1. **Batch upstream.** The per-PUBLISH cost (topic + TCP write + broker
   round-trip), not bytes, limits message rate — a CAN stream must
   coalesce many frames into ONE payload (the legacy JSON-array format:
   `{"bus","type","ts","frame":[…]}`) before publishing. That's the
   producer's/translator's job; this component moves opaque payloads.
2. **Use `publish_async`.** The ring absorbs broker/TCP hiccups (32 KB ≈
   dozens of batched messages); the producer never stalls — a CAN RX
   task keeps its deadline no matter what the network does.
3. **Watch `mqtt_manager_stats`.** Rising `dropped_full` means the link
   can't carry the offered rate — batch bigger or send less, don't grow
   the ring.

Live-verified end to end (2026-07-04): imu/battery events → main's
events glue → `publish_async` → mosquitto
(`{"event":"battery_below","voltage":11.51,"ts":"…"}` on
`<prefix>/events`).

## Settings (`"mqtt_manager"`, version 1, field table)

`enabled` (false) · `url` (`mqtt://host[:port]` or `mqtts://…`, validated)
· `username` · `broker_password` (the `_password` suffix auto-redacts it
in settings GETs) · `client_id` ("" = derived) · `topic_prefix` ("" =
derived) · `cert_set` ("" = none; a cert_manager set name — its CA
verifies the broker, and client cert+key when present = **mutual TLS**;
live-verified against the bench TLS broker) · `ca_file` ("" = built-in
certificate bundle; a raw PEM path — `cert_set` wins over it) ·
`keepalive_s` (5..600, 30). TLS priority: `cert_set` > `ca_file` >
bundle.

## Files

- `mqtt_manager.c` — lifecycle, settings, client glue, handler registry,
  the async ring + publisher task, network-gated starter.
- `mqtt_manager_match.c` — PURE topic-filter matcher + URL validation
  (host-tested).
- `mqtt_manager_item.c` — PURE ring-item codec (host-tested).

## Memory

32 KB PSRAM async ring + 4 KB PSRAM publisher stack; starter task 3 KB
PSRAM (self-deletes); esp-mqtt task + 4 KB in/out buffers (its own
allocs); optional 8 KB PSRAM CA buffer. (estimated)
